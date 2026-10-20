#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 2 Enhanced Client: Parallel TCP connections and byte range reassembly.
 *
 * Usage: client <server_ip> <server_port> <topic> <output_dir> [parallel_connections]
 *
 * The primary connection fetches the manifest; N worker threads download disjoint
 * byte ranges in parallel using ASSIGN_RANGE. Failed connections resume only their
 * unfinished ranges without retransmitting completed data.
 */

#include "client.h"
#include "../common/cli.h"
#include "../common/enhanced_protocol.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PARALLEL_CONNECTIONS 4u
#define RANGE_SLICE_SIZE (512u * 1024u) /* 512 KiB ranges for large files */

struct range_task {
    uint32_t range_id;
    uint32_t file_index;
    uint64_t start_offset;
    uint64_t end_offset;
    uint64_t current_offset;
    int completed;
    struct range_task *next;
};

struct file_target {
    char *name;
    uint64_t size;
    uint8_t sha256[CN_SHA256_SIZE];
    int fd;
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

    /* Metrics */
    pthread_mutex_t metrics_lock;
    uint64_t wire_bytes_received;
    uint64_t wire_bytes_sent;
    uint64_t data_bytes_received;
    uint32_t retransmissions_count;
};

static int connect_tcp(const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int sock_fd = -1;
    int opt = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0) continue;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sock_fd);
        sock_fd = -1;
    }
    freeaddrinfo(res);
    return sock_fd;
}

static void *worker_thread(void *arg)
{
    struct parallel_pool *pool = arg;

    while (1) {
        struct range_task *task = NULL;

        pthread_mutex_lock(&pool->queue_lock);
        task = pool->queue_head;
        while (task != NULL && task->completed) {
            task = task->next;
        }
        pthread_mutex_unlock(&pool->queue_lock);

        if (task == NULL) {
            break; /* No more unfinished ranges */
        }

        /* Download this range until completed or retry */
        while (task->current_offset < task->end_offset) {
            int sock = connect_tcp(pool->host, pool->port);
            if (sock < 0) {
                struct timespec delay = {0, 50000000L}; /* 50ms */
                nanosleep(&delay, NULL);
                continue;
            }

            struct cn_range_assignment assign;
            assign.range_id = task->range_id;
            assign.file_index = task->file_index;
            assign.start_offset = task->current_offset;
            assign.end_offset = task->end_offset;

            uint8_t payload[CN_ASSIGN_RANGE_PAYLOAD_SIZE];
            if (cn_encode_assign_range(&assign, payload) != 0) {
                close(sock);
                break;
            }

            struct cn_frame_header hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.version = CN_PROTOCOL_VERSION;
            hdr.message_type = CN_MSG_ASSIGN_RANGE;
            hdr.payload_length = sizeof(payload);
            memcpy(hdr.transfer_id, pool->transfer_id, CN_TRANSFER_ID_SIZE);

            if (cn_send_frame(sock, &hdr, payload) != CN_PROTOCOL_OK) {
                close(sock);
                continue;
            }

            pthread_mutex_lock(&pool->metrics_lock);
            pool->wire_bytes_sent += CN_FRAME_HEADER_SIZE + sizeof(payload);
            pthread_mutex_unlock(&pool->metrics_lock);

            /* Expect RANGE_READY */
            uint8_t *resp_payload = NULL;
            struct cn_frame_header resp_hdr;
            if (cn_recv_frame(sock, &resp_hdr, &resp_payload) != CN_PROTOCOL_OK ||
                resp_hdr.message_type != CN_MSG_RANGE_READY) {
                free(resp_payload);
                close(sock);
                continue;
            }

            pthread_mutex_lock(&pool->metrics_lock);
            pool->wire_bytes_received += CN_FRAME_HEADER_SIZE + resp_hdr.payload_length;
            pthread_mutex_unlock(&pool->metrics_lock);
            free(resp_payload);

            /* Receive DATA frames until RANGE_END or connection drop */
            int range_done = 0;
            while (task->current_offset < task->end_offset) {
                uint8_t *data_payload = NULL;
                struct cn_frame_header dhdr;
                enum cn_protocol_result pres = cn_recv_frame(sock, &dhdr, &data_payload);
                if (pres != CN_PROTOCOL_OK) {
                    /* Interrupted / cut connection: record retry */
                    pthread_mutex_lock(&pool->metrics_lock);
                    pool->retransmissions_count++;
                    pthread_mutex_unlock(&pool->metrics_lock);
                    free(data_payload);
                    break;
                }

                pthread_mutex_lock(&pool->metrics_lock);
                pool->wire_bytes_received += CN_FRAME_HEADER_SIZE + dhdr.payload_length;
                pthread_mutex_unlock(&pool->metrics_lock);

                if (dhdr.message_type == CN_MSG_RANGE_END) {
                    range_done = 1;
                    free(data_payload);
                    break;
                }

                if (dhdr.message_type == CN_MSG_DATA) {
                    struct cn_reader reader;
                    uint32_t findex;
                    uint64_t offset;
                    uint32_t data_len;

                    cn_reader_init(&reader, data_payload, (size_t)dhdr.payload_length);
                    if (cn_reader_get_u32(&reader, &findex) == 0 &&
                        cn_reader_get_u64(&reader, &offset) == 0 &&
                        cn_reader_get_u32(&reader, &data_len) == 0) {
                        const uint8_t *chunk = data_payload + 16;
                        size_t chunk_bytes = (size_t)dhdr.payload_length > 16 ?
                                             (size_t)dhdr.payload_length - 16 : 0;
                        if (chunk_bytes > 0 && findex == task->file_index) {
                            pwrite(pool->files[findex].fd, chunk, chunk_bytes, (off_t)offset);
                            task->current_offset = offset + chunk_bytes;

                            pthread_mutex_lock(&pool->metrics_lock);
                            pool->data_bytes_received += chunk_bytes;
                            pthread_mutex_unlock(&pool->metrics_lock);
                        }
                    }
                }
                free(data_payload);
            }

            close(sock);

            if (range_done || task->current_offset >= task->end_offset) {
                pthread_mutex_lock(&pool->queue_lock);
                task->completed = 1;
                pool->completed_ranges++;

                /* Check if file is finished */
                struct file_target *f = &pool->files[task->file_index];
                if (f->pending_ranges > 0) {
                    f->pending_ranges--;
                    if (f->pending_ranges == 0) {
                        /* Verify SHA-256 and commit */
                        uint8_t digest[CN_SHA256_SIZE];
                        fsync(f->fd);
                        if (cn_sha256_fd(f->fd, digest) == 0 &&
                            memcmp(digest, f->sha256, CN_SHA256_SIZE) == 0) {
                            close(f->fd);
                            f->fd = -1;
                            (void)rename(f->part_path, f->final_path);
                            f->verified = 1;
                        }
                    }
                }
                pthread_mutex_unlock(&pool->queue_lock);
                break;
            }
        }
    }

    return NULL;
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
    int primary_sock = -1;
    uint8_t *manifest_payload = NULL;
    struct cn_frame_header mhdr;
    struct cn_manifest manifest;
    const char *manifest_reason = NULL;
    size_t i;
    int dir_fd = -1;

    if (argc < 5) {
        fprintf(stderr, "usage: client <server_ip> <server_port> <topic> <output_dir> [parallel_connections]\n");
        return 1;
    }
    host = argv[1];
    port = (uint16_t)atoi(argv[2]);
    topic = argv[3];
    output_dir = argv[4];

    if (argc >= 6) {
        num_workers = (uint32_t)atoi(argv[5]);
        if (num_workers < 1) num_workers = 1;
        if (num_workers > 64) num_workers = 64;
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    signal(SIGPIPE, SIG_IGN);

    /* Generate random transfer ID */
    srand((unsigned)time(NULL));
    for (i = 0; i < CN_TRANSFER_ID_SIZE; ++i) {
        pool.transfer_id[i] = (uint8_t)(rand() & 0xff);
    }

    /* Primary connection to fetch MANIFEST */
    primary_sock = connect_tcp(host, port);
    if (primary_sock < 0) {
        fprintf(stderr, "client: cannot connect to server %s:%u\n", host, (unsigned)port);
        return 3;
    }

    /* Send GET <topic> */
    {
        uint8_t get_payload[4 + CN_MAX_TOPIC_SIZE];
        struct cn_writer writer;
        struct cn_frame_header ghdr;

        cn_writer_init(&writer, get_payload, sizeof(get_payload));
        cn_writer_put_string(&writer, topic, CN_MAX_TOPIC_SIZE);

        memset(&ghdr, 0, sizeof(ghdr));
        ghdr.version = CN_PROTOCOL_VERSION;
        ghdr.message_type = CN_MSG_GET;
        ghdr.payload_length = writer.offset;
        memcpy(ghdr.transfer_id, pool.transfer_id, CN_TRANSFER_ID_SIZE);

        if (cn_send_frame(primary_sock, &ghdr, get_payload) != CN_PROTOCOL_OK) {
            close(primary_sock);
            return 3;
        }
    }

    /* Receive MANIFEST */
    if (cn_recv_frame(primary_sock, &mhdr, &manifest_payload) != CN_PROTOCOL_OK ||
        mhdr.message_type != CN_MSG_MANIFEST) {
        free(manifest_payload);
        close(primary_sock);
        return 4;
    }
    close(primary_sock);

    if (cn_manifest_parse(manifest_payload, (size_t)mhdr.payload_length, &manifest, &manifest_reason) != 0) {
        free(manifest_payload);
        return 4;
    }
    free(manifest_payload);

    /* Create output directory */
    dir_fd = cn_client_open_topic_dir(output_dir, topic);
    if (dir_fd < 0) {
        cn_manifest_free(&manifest);
        return 6;
    }
    close(dir_fd);

    /* Initialize pool */
    pool.host = host;
    pool.port = port;
    pool.topic = topic;
    pool.output_dir = output_dir;
    pool.num_workers = num_workers;
    pool.file_count = manifest.count;
    pool.total_topic_bytes = manifest.total_bytes;
    pool.queue_head = NULL;
    pool.total_ranges = 0;
    pool.completed_ranges = 0;
    pool.wire_bytes_received = CN_FRAME_HEADER_SIZE + mhdr.payload_length;
    pool.wire_bytes_sent = CN_FRAME_HEADER_SIZE + 4 + strlen(topic);
    pool.data_bytes_received = 0;
    pool.retransmissions_count = 0;
    pthread_mutex_init(&pool.queue_lock, NULL);
    pthread_mutex_init(&pool.metrics_lock, NULL);

    pool.files = calloc(manifest.count, sizeof(struct file_target));
    if (pool.files == NULL) {
        cn_manifest_free(&manifest);
        return 6;
    }

    uint32_t range_counter = 1;
    struct range_task **tail_ptr = &pool.queue_head;

    char topic_dir[1024];
    (void)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", output_dir, topic);

    for (i = 0; i < manifest.count; ++i) {
        struct file_target *f = &pool.files[i];
        f->name = strdup(manifest.entries[i].name);
        f->size = manifest.entries[i].size;
        memcpy(f->sha256, manifest.entries[i].sha256, CN_SHA256_SIZE);

        char part_buf[2048], final_buf[2048];
        (void)snprintf(part_buf, sizeof(part_buf), "%s/.%s.part", topic_dir, f->name);
        (void)snprintf(final_buf, sizeof(final_buf), "%s/%s", topic_dir, f->name);
        f->part_path = strdup(part_buf);
        f->final_path = strdup(final_buf);

        f->fd = open(f->part_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (f->fd >= 0 && f->size > 0) {
            (void)ftruncate(f->fd, (off_t)f->size);
        }

        /* Partition into ranges */
        if (f->size == 0) {
            f->pending_ranges = 0;
            f->verified = 1;
            close(f->fd);
            f->fd = -1;
            (void)rename(f->part_path, f->final_path);
            continue;
        }

        uint64_t offset = 0;
        uint32_t file_ranges = 0;
        while (offset < f->size) {
            uint64_t end = offset + RANGE_SLICE_SIZE;
            if (end > f->size) end = f->size;

            struct range_task *t = malloc(sizeof(*t));
            t->range_id = range_counter++;
            t->file_index = (uint32_t)i;
            t->start_offset = offset;
            t->end_offset = end;
            t->current_offset = offset;
            t->completed = 0;
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
    pthread_t *threads = malloc(sizeof(pthread_t) * num_workers);
    for (i = 0; i < num_workers; ++i) {
        pthread_create(&threads[i], NULL, worker_thread, &pool);
    }

    for (i = 0; i < num_workers; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (double)(end_time.tv_sec - start_time.tv_sec) +
                     (double)(end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    /* Verify all files succeeded */
    uint32_t completed_files = 0;
    for (i = 0; i < manifest.count; ++i) {
        if (pool.files[i].fd >= 0) {
            close(pool.files[i].fd);
            pool.files[i].fd = -1;
        }
        if (pool.files[i].verified) {
            completed_files++;
        }
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

    return (completed_files == manifest.count) ? 0 : 5;
}
