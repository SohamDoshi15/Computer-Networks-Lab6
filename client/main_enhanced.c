#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 2 Enhanced Client: Parallel TCP connections and byte range reassembly.
 *
 * Usage: client <server_ip> <server_port> <topic> <output_dir> [parallel_connections]
 *
 * The primary connection fetches the manifest; N worker threads download disjoint
 * byte ranges in parallel using ASSIGN_RANGE. Failed connections resume only their
 * unfinished ranges without retransmitting completed data.
 *
 * Exit codes follow enum cn_client_status (see README): 2 when the server
 * answers with ERROR, 3 when a range makes no progress for RANGE_GIVEUP_SECONDS.
 */

#include "client.h"
#include "../common/cli.h"
#include "../common/enhanced_protocol.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PARALLEL_CONNECTIONS 4u
#define MAX_PARALLEL_CONNECTIONS 64u
#define RANGE_SLICE_SIZE (512u * 1024u) /* 512 KiB ranges for large files */
#define TIMEOUT_SECONDS 30u             /* per blocking send and recv, as in the other clients */
#define RANGE_GIVEUP_SECONDS 30u        /* retrying one range without any progress */
#define RETRY_DELAY_MS 50u
#define RETRY_DELAY_MAX_MS 2000u

struct range_task {
    uint32_t range_id;
    uint32_t file_index;
    uint64_t start_offset;
    uint64_t end_offset;
    uint64_t current_offset;
    int completed;
    int in_progress;
    struct range_task *next;
};

struct file_target {
    char *name;
    uint64_t size;
    uint8_t sha256[CN_SHA256_SIZE];
    char *part_path;
    char *final_path;
    uint32_t pending_ranges;
    int verified;
};

struct parallel_pool {
    const char *host;
    uint16_t port;
    const char *topic;
    const char *output_dir;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    uint32_t num_workers;

    struct file_target *files;
    uint32_t file_count;
    uint64_t total_topic_bytes;

    pthread_mutex_t queue_lock;
    struct range_task *queue_head;
    uint32_t total_ranges;
    uint32_t completed_ranges;
    /* First unrecoverable failure; workers stop taking ranges once it is set. */
    enum cn_client_status status;
    char message[512];

    /* Metrics */
    pthread_mutex_t metrics_lock;
    uint64_t wire_bytes_received;
    uint64_t wire_bytes_sent;
    uint64_t data_bytes_received;
    uint32_t retransmissions_count;
};

static double seconds_since(const struct timespec *start)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void sleep_ms(unsigned ms)
{
    struct timespec pause;
    pause.tv_sec = (time_t)(ms / 1000u);
    pause.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

static void pool_fail(struct parallel_pool *pool, enum cn_client_status status,
                      const char *format, ...)
{
    va_list args;

    pthread_mutex_lock(&pool->queue_lock);
    if (pool->status == CN_CLIENT_OK) {
        pool->status = status;
        va_start(args, format);
        (void)vsnprintf(pool->message, sizeof(pool->message), format, args);
        va_end(args);
    }
    pthread_mutex_unlock(&pool->queue_lock);
}

static int pool_failed(struct parallel_pool *pool)
{
    int failed;

    pthread_mutex_lock(&pool->queue_lock);
    failed = pool->status != CN_CLIENT_OK;
    pthread_mutex_unlock(&pool->queue_lock);
    return failed;
}

static int pwrite_all(int fd, const uint8_t *buffer, size_t length, off_t offset)
{
    size_t written = 0;

    while (written < length) {
        ssize_t rc = pwrite(fd, buffer + written, length - written,
                            offset + (off_t)written);
        if (rc > 0) {
            written += (size_t)rc;
        } else if (rc < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int send_frame(int fd, uint8_t type, const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                      const uint8_t *payload, size_t length)
{
    struct cn_frame_header header;

    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = type;
    header.payload_length = length;
    memcpy(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
    return cn_send_frame(fd, &header, payload);
}

/* Receives one frame of this transfer. An ERROR frame becomes CN_CLIENT_SERVER_ERROR.
   On any failure *payload is NULL and message says why. */
static enum cn_client_status recv_transfer_frame(int sock, const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                                 struct cn_frame_header *hdr, uint8_t **payload,
                                                 char *message, size_t message_size)
{
    const int rc = cn_recv_frame(sock, hdr, payload);

    if (rc == CN_PROTOCOL_NO_MEMORY) {
        (void)snprintf(message, message_size, "out of memory while receiving a frame");
        return CN_CLIENT_LOCAL_IO;
    }
    if (rc == CN_PROTOCOL_INVALID || rc == CN_PROTOCOL_TOO_LARGE) {
        (void)snprintf(message, message_size, "malformed frame header from server");
        return CN_CLIENT_PROTOCOL;
    }
    if (rc != CN_PROTOCOL_OK) {
        (void)snprintf(message, message_size, "connection lost: %s",
                       rc == CN_PROTOCOL_IO_ERROR ? strerror(errno) : "server disconnected");
        return CN_CLIENT_NETWORK;
    }
    if (memcmp(hdr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
        free(*payload);
        *payload = NULL;
        (void)snprintf(message, message_size, "frame carries a different transfer ID");
        return CN_CLIENT_PROTOCOL;
    }
    if (hdr->message_type == CN_MSG_ERROR) {
        char text[CN_MAX_ERROR_TEXT_SIZE + 1] = "";
        struct cn_reader reader;
        uint32_t code = 0;
        size_t i;

        cn_reader_init(&reader, *payload, (size_t)hdr->payload_length);
        (void)cn_reader_get_u32(&reader, &code);
        (void)cn_reader_get_string(&reader, text, sizeof(text), CN_MAX_ERROR_TEXT_SIZE);
        /* The text is untrusted; keep control characters away from the terminal. */
        for (i = 0; text[i] != '\0'; ++i) {
            if ((unsigned char)text[i] < 32 || text[i] == 127) {
                text[i] = '?';
            }
        }
        free(*payload);
        *payload = NULL;
        (void)snprintf(message, message_size, "server error %" PRIu32 ": %.400s", code, text);
        return CN_CLIENT_SERVER_ERROR;
    }
    return CN_CLIENT_OK;
}

static enum cn_client_status write_range_data(struct parallel_pool *pool, struct range_task *task,
                                              int fd, const uint8_t *payload, size_t length,
                                              char *message, size_t message_size)
{
    struct cn_reader reader;
    uint32_t file_index, data_length;
    uint64_t offset;

    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &file_index) != 0 ||
        cn_reader_get_u64(&reader, &offset) != 0 ||
        cn_reader_get_u32(&reader, &data_length) != 0 ||
        data_length != cn_reader_remaining(&reader) || data_length == 0 ||
        file_index != task->file_index || offset != task->current_offset ||
        data_length > task->end_offset - task->current_offset) {
        (void)snprintf(message, message_size, "DATA frame outside range %" PRIu32, task->range_id);
        return CN_CLIENT_PROTOCOL;
    }
    if (pwrite_all(fd, payload + reader.offset, data_length, (off_t)offset) != 0) {
        (void)snprintf(message, message_size, "cannot write %s: %s",
                       pool->files[task->file_index].name, strerror(errno));
        return CN_CLIENT_LOCAL_IO;
    }
    task->current_offset += data_length;

    pthread_mutex_lock(&pool->metrics_lock);
    pool->data_bytes_received += data_length;
    pthread_mutex_unlock(&pool->metrics_lock);
    return CN_CLIENT_OK;
}

/* One connection: ASSIGN_RANGE for the unfinished part of the range, then DATA
   until RANGE_END. Progress is kept in task->current_offset even on failure;
   *answered tells whether the server accepted the range before it failed. */
static enum cn_client_status range_attempt(struct parallel_pool *pool, struct range_task *task,
                                           int fd, int *answered, char *message, size_t message_size)
{
    struct cn_client_report report;
    struct cn_range_assignment assign, ready;
    struct cn_range_end end;
    struct cn_frame_header hdr;
    uint8_t request[CN_ASSIGN_RANGE_PAYLOAD_SIZE];
    uint8_t *payload = NULL;
    enum cn_client_status status;
    int sock;

    *answered = 0;
    memset(&report, 0, sizeof(report));
    sock = cn_client_connect(pool->host, pool->port, TIMEOUT_SECONDS, &report);
    if (sock < 0) {
        (void)snprintf(message, message_size, "%s", report.message);
        return CN_CLIENT_NETWORK;
    }

    assign.range_id = task->range_id;
    assign.file_index = task->file_index;
    assign.start_offset = task->current_offset;
    assign.end_offset = task->end_offset;
    if (cn_encode_assign_range(&assign, request) != 0 ||
        send_frame(sock, CN_MSG_ASSIGN_RANGE, pool->transfer_id, request, sizeof(request)) != CN_PROTOCOL_OK) {
        (void)snprintf(message, message_size, "cannot send ASSIGN_RANGE: %s", strerror(errno));
        status = CN_CLIENT_NETWORK;
        goto done;
    }

    status = recv_transfer_frame(sock, pool->transfer_id, &hdr, &payload, message, message_size);
    if (status != CN_CLIENT_OK) {
        goto done;
    }
    if (hdr.message_type != CN_MSG_RANGE_READY ||
        cn_decode_range_ready(payload, (size_t)hdr.payload_length, &ready) != 0 ||
        ready.range_id != assign.range_id || ready.file_index != assign.file_index ||
        ready.start_offset != assign.start_offset || ready.end_offset != assign.end_offset) {
        (void)snprintf(message, message_size, "expected RANGE_READY for range %" PRIu32, assign.range_id);
        status = CN_CLIENT_PROTOCOL;
        goto done;
    }
    free(payload);
    payload = NULL;
    *answered = 1;

    for (;;) {
        status = recv_transfer_frame(sock, pool->transfer_id, &hdr, &payload, message, message_size);
        if (status != CN_CLIENT_OK) {
            goto done;
        }
        if (hdr.message_type == CN_MSG_DATA) {
            status = write_range_data(pool, task, fd, payload, (size_t)hdr.payload_length,
                                      message, message_size);
            free(payload);
            payload = NULL;
            if (status != CN_CLIENT_OK) {
                goto done;
            }
            continue;
        }
        if (hdr.message_type == CN_MSG_RANGE_END &&
            cn_decode_range_end(payload, (size_t)hdr.payload_length, &end) == 0 &&
            end.range_id == task->range_id && end.file_index == task->file_index &&
            end.end_offset == task->end_offset && task->current_offset == task->end_offset) {
            goto done;
        }
        (void)snprintf(message, message_size, "unexpected frame %u in range %" PRIu32 " at offset %" PRIu64,
                       (unsigned)hdr.message_type, task->range_id, task->current_offset);
        status = CN_CLIENT_PROTOCOL;
        goto done;
    }

done:
    free(payload);
    close(sock);
    return status;
}

/* Marks the range done; the worker that finishes a file's last range verifies it. */
static enum cn_client_status finish_range(struct parallel_pool *pool, struct range_task *task)
{
    struct file_target *f = &pool->files[task->file_index];
    uint8_t digest[CN_SHA256_SIZE];
    int last = 0, fd, hashed;

    pthread_mutex_lock(&pool->queue_lock);
    task->completed = 1;
    task->in_progress = 0;
    pool->completed_ranges++;
    if (f->pending_ranges > 0) {
        f->pending_ranges--;
        last = f->pending_ranges == 0;
    }
    pthread_mutex_unlock(&pool->queue_lock);
    if (!last) {
        return CN_CLIENT_OK;
    }

    /* Every range of this file is written; hash outside the queue lock so the
       other workers keep taking ranges meanwhile. */
    fd = open(f->part_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    hashed = fd >= 0 && fsync(fd) == 0 && cn_sha256_fd(fd, digest) == 0;
    if (fd >= 0) {
        close(fd);
    }
    if (!hashed) {
        pool_fail(pool, CN_CLIENT_LOCAL_IO, "cannot verify %s: %s", f->name, strerror(errno));
        return CN_CLIENT_LOCAL_IO;
    }
    if (memcmp(digest, f->sha256, CN_SHA256_SIZE) != 0) {
        pool_fail(pool, CN_CLIENT_INTEGRITY, "%s: SHA-256 mismatch", f->name);
        return CN_CLIENT_INTEGRITY;
    }
    if (rename(f->part_path, f->final_path) != 0) {
        pool_fail(pool, CN_CLIENT_LOCAL_IO, "cannot rename %s: %s", f->name, strerror(errno));
        return CN_CLIENT_LOCAL_IO;
    }
    f->verified = 1;
    return CN_CLIENT_OK;
}

/* Downloads one range, reconnecting after cuts, and gives up only when the
   range makes no progress for RANGE_GIVEUP_SECONDS or the server rejects it. */
static enum cn_client_status download_range(struct parallel_pool *pool, struct range_task *task)
{
    struct file_target *f = &pool->files[task->file_index];
    struct timespec last_progress;
    unsigned delay_ms = RETRY_DELAY_MS;
    enum cn_client_status status;
    char message[512] = "";
    int answered = 0, fd;

    /* Opened per range, so a topic of thousands of files needs only N descriptors. */
    fd = open(f->part_path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        pool_fail(pool, CN_CLIENT_LOCAL_IO, "cannot open %s: %s", f->part_path, strerror(errno));
        return CN_CLIENT_LOCAL_IO;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &last_progress);
    for (;;) {
        const uint64_t before = task->current_offset;

        if (pool_failed(pool)) {
            close(fd);
            return CN_CLIENT_NETWORK; /* another worker already recorded why */
        }
        status = range_attempt(pool, task, fd, &answered, message, sizeof(message));
        /* Every byte is written and was checked: done even if RANGE_END was cut off. */
        if (task->current_offset == task->end_offset) {
            status = CN_CLIENT_OK;
            break;
        }
        if (status == CN_CLIENT_SERVER_ERROR || status == CN_CLIENT_LOCAL_IO) {
            break;
        }
        if (task->current_offset != before) {
            (void)clock_gettime(CLOCK_MONOTONIC, &last_progress);
        } else if (seconds_since(&last_progress) >= (double)RANGE_GIVEUP_SECONDS) {
            break;
        }

        pthread_mutex_lock(&pool->metrics_lock);
        pool->retransmissions_count++;
        pthread_mutex_unlock(&pool->metrics_lock);
        if (answered) {
            /* The server is up and the range was cut: reconnect at once. */
            delay_ms = RETRY_DELAY_MS;
        } else {
            /* Unreachable or silent server: back off instead of spinning. */
            sleep_ms(delay_ms);
            delay_ms = delay_ms * 2u > RETRY_DELAY_MAX_MS ? RETRY_DELAY_MAX_MS : delay_ms * 2u;
        }
    }
    close(fd);

    if (status != CN_CLIENT_OK) {
        pool_fail(pool, status, "range %" PRIu32 " of %s: %s", task->range_id, f->name, message);
        return status;
    }
    return finish_range(pool, task);
}

static void *worker_thread(void *arg)
{
    struct parallel_pool *pool = arg;
    struct cn_io_counters io = {0, 0};

    /* Counts every byte on this worker's sockets, including frames cut short. */
    cn_io_set_counters(&io);
    for (;;) {
        struct range_task *task = NULL;

        pthread_mutex_lock(&pool->queue_lock);
        if (pool->status == CN_CLIENT_OK) {
            task = pool->queue_head;
            while (task != NULL && (task->completed || task->in_progress)) {
                task = task->next;
            }
            if (task != NULL) {
                /* A task stays owned by this worker across reconnect attempts. */
                task->in_progress = 1;
            }
        }
        pthread_mutex_unlock(&pool->queue_lock);

        if (task == NULL || download_range(pool, task) != CN_CLIENT_OK) {
            break; /* no more unfinished ranges, or the transfer has failed */
        }
    }
    cn_io_set_counters(NULL);

    pthread_mutex_lock(&pool->metrics_lock);
    pool->wire_bytes_received += io.bytes_received;
    pool->wire_bytes_sent += io.bytes_sent;
    pthread_mutex_unlock(&pool->metrics_lock);
    return NULL;
}

/* Sends GET on a fresh connection and returns the parsed MANIFEST. */
static enum cn_client_status fetch_manifest(struct parallel_pool *pool, struct cn_manifest *manifest,
                                            char *message, size_t message_size)
{
    struct cn_client_report report;
    struct cn_frame_header hdr;
    struct cn_writer writer;
    uint8_t get_payload[4 + CN_MAX_TOPIC_SIZE];
    uint8_t *payload = NULL;
    const char *reason = NULL;
    enum cn_client_status status;
    int sock;

    memset(&report, 0, sizeof(report));
    sock = cn_client_connect(pool->host, pool->port, TIMEOUT_SECONDS, &report);
    if (sock < 0) {
        (void)snprintf(message, message_size, "%s", report.message);
        return CN_CLIENT_NETWORK;
    }

    cn_writer_init(&writer, get_payload, sizeof(get_payload));
    if (cn_writer_put_string(&writer, pool->topic, CN_MAX_TOPIC_SIZE) != 0 ||
        send_frame(sock, CN_MSG_GET, pool->transfer_id, get_payload, writer.offset) != CN_PROTOCOL_OK) {
        (void)snprintf(message, message_size, "cannot send GET: %s", strerror(errno));
        close(sock);
        return CN_CLIENT_NETWORK;
    }

    status = recv_transfer_frame(sock, pool->transfer_id, &hdr, &payload, message, message_size);
    close(sock);
    if (status != CN_CLIENT_OK) {
        return status;
    }
    if (hdr.message_type != CN_MSG_MANIFEST ||
        cn_manifest_parse(payload, (size_t)hdr.payload_length, manifest, &reason) != 0) {
        (void)snprintf(message, message_size, "invalid MANIFEST: %s",
                       reason != NULL ? reason : "unexpected frame");
        free(payload);
        return CN_CLIENT_PROTOCOL;
    }
    free(payload);
    return CN_CLIENT_OK;
}

int main(int argc, char **argv)
{
    const char *host;
    uint16_t port;
    const char *topic;
    const char *output_dir;
    uint32_t num_workers = DEFAULT_PARALLEL_CONNECTIONS;
    struct parallel_pool pool;
    struct timespec start_time, end_time;
    struct cn_manifest manifest = {NULL, 0, 0};
    struct cn_io_counters primary_io = {0, 0};
    enum cn_client_status status;
    char message[512] = "";
    pthread_t *threads;
    uint32_t started = 0;
    size_t i;
    int dir_fd = -1;

    if (argc < 5 || argc > 6 || cn_parse_port(argv[2], &port) != 0 || argv[4][0] == '\0' ||
        (argc == 6 && cn_parse_positive_u32(argv[5], &num_workers) != 0)) {
        fprintf(stderr, "usage: client <server_ip> <server_port> <topic> <output_dir> [parallel_connections]\n");
        return CN_CLIENT_USAGE;
    }
    host = argv[1];
    topic = argv[3];
    output_dir = argv[4];
    if (!cn_client_name_is_valid(topic, CN_MAX_TOPIC_SIZE)) {
        fprintf(stderr, "client: invalid topic name: use 1-%u bytes without '/', '\\' or "
                        "control characters, and not '.' or '..'\n", CN_MAX_TOPIC_SIZE);
        return CN_CLIENT_USAGE;
    }
    if (num_workers > MAX_PARALLEL_CONNECTIONS) num_workers = MAX_PARALLEL_CONNECTIONS;

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    signal(SIGPIPE, SIG_IGN);

    memset(&pool, 0, sizeof(pool));
    pool.host = host;
    pool.port = port;
    pool.topic = topic;
    pool.output_dir = output_dir;
    pool.num_workers = num_workers;
    pool.status = CN_CLIENT_OK;

    /* Random, not time-seeded: clients started in the same second must not share a transfer. */
    if (cn_client_new_transfer_id(pool.transfer_id) != 0) {
        perror("client: cannot generate a transfer ID");
        return CN_CLIENT_LOCAL_IO;
    }

    /* Primary connection to fetch MANIFEST */
    cn_io_set_counters(&primary_io);
    status = fetch_manifest(&pool, &manifest, message, sizeof(message));
    cn_io_set_counters(NULL);
    if (status != CN_CLIENT_OK) {
        fprintf(stderr, "client: %s\n", message);
        return (int)status;
    }

    /* Create output directory */
    dir_fd = cn_client_open_topic_dir(output_dir, topic);
    if (dir_fd < 0) {
        fprintf(stderr, "client: cannot create %s/%s: %s\n", output_dir, topic, strerror(errno));
        cn_manifest_free(&manifest);
        return CN_CLIENT_LOCAL_IO;
    }
    close(dir_fd);

    /* Initialize pool */
    pool.file_count = manifest.count;
    pool.total_topic_bytes = manifest.total_bytes;
    pool.wire_bytes_received = primary_io.bytes_received;
    pool.wire_bytes_sent = primary_io.bytes_sent;
    pthread_mutex_init(&pool.queue_lock, NULL);
    pthread_mutex_init(&pool.metrics_lock, NULL);

    pool.files = calloc(manifest.count == 0 ? 1 : manifest.count, sizeof(struct file_target));
    if (pool.files == NULL) {
        cn_manifest_free(&manifest);
        return CN_CLIENT_LOCAL_IO;
    }

    uint32_t range_counter = 1;
    struct range_task **tail_ptr = &pool.queue_head;

    char topic_dir[1024];
    (void)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", output_dir, topic);

    for (i = 0; i < manifest.count && pool.status == CN_CLIENT_OK; ++i) {
        struct file_target *f = &pool.files[i];
        int fd;
        f->name = strdup(manifest.entries[i].name);
        f->size = manifest.entries[i].size;
        memcpy(f->sha256, manifest.entries[i].sha256, CN_SHA256_SIZE);

        char part_buf[2048], final_buf[2048];
        (void)snprintf(part_buf, sizeof(part_buf), "%s/.%s.part", topic_dir, manifest.entries[i].name);
        (void)snprintf(final_buf, sizeof(final_buf), "%s/%s", topic_dir, manifest.entries[i].name);
        f->part_path = strdup(part_buf);
        f->final_path = strdup(final_buf);
        if (f->name == NULL || f->part_path == NULL || f->final_path == NULL) {
            pool_fail(&pool, CN_CLIENT_LOCAL_IO, "out of memory");
            break;
        }

        /* Pre-size the file so workers can pwrite() any range; closed again so the
           number of open descriptors does not grow with the topic. */
        fd = open(f->part_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0 || (f->size > 0 && ftruncate(fd, (off_t)f->size) != 0)) {
            pool_fail(&pool, CN_CLIENT_LOCAL_IO, "failed to prepare output for %s: %s",
                      f->name, strerror(errno));
            if (fd >= 0) {
                close(fd);
            }
            break;
        }
        close(fd);

        /* Partition into ranges */
        if (f->size == 0) {
            f->pending_ranges = 0;
            f->verified = 1;
            (void)rename(f->part_path, f->final_path);
            continue;
        }

        uint64_t offset = 0;
        uint32_t file_ranges = 0;
        while (offset < f->size) {
            uint64_t end = offset + RANGE_SLICE_SIZE;
            if (end > f->size) end = f->size;

            struct range_task *t = malloc(sizeof(*t));
            if (t == NULL) {
                pool_fail(&pool, CN_CLIENT_LOCAL_IO, "out of memory");
                break;
            }
            t->range_id = range_counter++;
            t->file_index = (uint32_t)i;
            t->start_offset = offset;
            t->end_offset = end;
            t->current_offset = offset;
            t->completed = 0;
            t->in_progress = 0;
            t->next = NULL;

            *tail_ptr = t;
            tail_ptr = &t->next;

            file_ranges++;
            offset = end;
        }
        f->pending_ranges = file_ranges;
        pool.total_ranges += file_ranges;
    }

    /* Spawn N worker threads */
    threads = malloc(sizeof(pthread_t) * num_workers);
    if (threads == NULL) {
        pool_fail(&pool, CN_CLIENT_LOCAL_IO, "out of memory");
    } else {
        while (started < num_workers && pool.status == CN_CLIENT_OK &&
               pthread_create(&threads[started], NULL, worker_thread, &pool) == 0) {
            started++;
        }
        if (started == 0) {
            pool_fail(&pool, CN_CLIENT_LOCAL_IO, "cannot start worker threads");
        }
        for (i = 0; i < started; ++i) {
            pthread_join(threads[i], NULL);
        }
        free(threads);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (double)(end_time.tv_sec - start_time.tv_sec) +
                     (double)(end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    /* Verify all files succeeded */
    uint32_t completed_files = 0;
    for (i = 0; i < manifest.count; ++i) {
        if (pool.files[i].verified) {
            completed_files++;
        }
    }
    status = pool.status;
    if (status == CN_CLIENT_OK && completed_files != manifest.count) {
        status = CN_CLIENT_INTEGRITY;
        (void)snprintf(pool.message, sizeof(pool.message), "only %" PRIu32 " of %" PRIu32 " files verified",
                       completed_files, manifest.count);
    }
    if (status != CN_CLIENT_OK) {
        fprintf(stderr, "client: %s\n", pool.message);
    }

    uint64_t useful_bytes = pool.total_topic_bytes;
    uint64_t redundant_bytes = (pool.data_bytes_received > useful_bytes) ?
                               (pool.data_bytes_received - useful_bytes) : 0;
    double mbps = elapsed > 0.0 ? ((double)useful_bytes / elapsed / 1e6) : 0.0;
    double retransmission_ratio = useful_bytes > 0 ? (double)redundant_bytes / (double)useful_bytes : 0.0;
    double overhead_ratio = useful_bytes > 0 ? (double)pool.wire_bytes_received / (double)useful_bytes : 1.0;

    printf("METRICS attempts=1 files=%" PRIu32 "/%" PRIu32 " wire_bytes_received=%" PRIu64
           " wire_bytes_sent=%" PRIu64 " data_bytes_received=%" PRIu64
           " useful_bytes=%" PRIu64 " final_wire_bytes=%" PRIu64
           " seconds=%.6f throughput_mbps=%.2f redundant_bytes=%" PRIu64
           " retransmission_ratio=%.4f overhead_ratio=%.4f parallel_connections=%" PRIu32 "\n",
           completed_files, manifest.count, pool.wire_bytes_received, pool.wire_bytes_sent,
           pool.data_bytes_received, useful_bytes, pool.wire_bytes_received,
           elapsed, mbps, redundant_bytes, retransmission_ratio, overhead_ratio, num_workers);

    /* Free structures */
    for (i = 0; i < manifest.count; ++i) {
        free(pool.files[i].name);
        free(pool.files[i].part_path);
        free(pool.files[i].final_path);
    }
    free(pool.files);

    struct range_task *curr = pool.queue_head;
    while (curr != NULL) {
        struct range_task *next = curr->next;
        free(curr);
        curr = next;
    }
    cn_manifest_free(&manifest);
    pthread_mutex_destroy(&pool.queue_lock);
    pthread_mutex_destroy(&pool.metrics_lock);

    return (int)status;
}
