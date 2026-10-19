#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 2 server: thread-per-client, in-memory session management,
 * and fault injection. The server maintains active sessions in a thread-safe
 * SessionStore so clients can resume interrupted transfers from their last
 * acknowledged checkpoint without retransmitting already delivered files or bytes.
 *
 * Usage: server <port> <topics_root_dir> <failure_probability>
 */

#include "fault_injection.h"
#include "server_common.h"
#include "session_store.h"
#include "../common/case2_protocol.h"
#include "../common/cli.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DATA_CHUNK_SIZE (64u * 1024u)

struct worker_args {
    int client_fd;
    uint64_t connection_id;
    const char *root;
    const struct cn_fault_config *fault_config;
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *metrics_csv = NULL;
static struct cn_session_store g_session_store;

static const char *outcome(const struct cn_serve_stats *stats)
{
    if (stats->completed) return "complete";
    if (stats->fault_fired) return "injected_fault";
    return "failed";
}

static double elapsed_since(const struct timespec *start)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void record_connection(const struct worker_args *args,
                              const struct cn_fault_injector *fault,
                              const struct cn_serve_stats *stats)
{
    const char *topic = stats->topic[0] != '\0' ? stats->topic : "-";
    double mbps = stats->seconds > 0.0 ? (double)stats->io.bytes_sent / stats->seconds / 1e6 : 0.0;

    pthread_mutex_lock(&log_lock);
    printf("conn=%" PRIu64 " topic=%s result=%s files_acked=%" PRIu32 "/%" PRIu32
           " data_sent=%" PRIu64 "/%" PRIu64 " wire_sent=%" PRIu64 " wire_received=%" PRIu64
           " seconds=%.6f send_rate=%.2fMB/s",
           args->connection_id, topic, outcome(stats), stats->files_acked, stats->file_count,
           stats->data_bytes_sent, stats->data_bytes_planned, stats->io.bytes_sent,
           stats->io.bytes_received, stats->seconds, mbps);
    if (fault->fired) {
        printf(" cut_at_data_byte=%" PRIu64, fault->cut_at);
    }
    printf("\n");
    fflush(stdout);
    if (metrics_csv != NULL) {
        fprintf(metrics_csv,
                "%" PRIu64 ",%s,%s,%.4f,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%.6f,%d,%" PRIu64 "\n",
                args->connection_id, topic, outcome(stats), args->fault_config->probability,
                cn_fault_mode_name(args->fault_config->mode), stats->files_acked,
                stats->file_count, stats->data_bytes_sent, stats->data_bytes_planned,
                stats->io.bytes_sent, stats->io.bytes_received, stats->seconds, fault->fired,
                fault->fired ? fault->cut_at : 0);
        fflush(metrics_csv);
    }
    pthread_mutex_unlock(&log_lock);
}

static int send_frame(int fd, uint8_t message_type, const uint8_t transfer_id[16],
                      const uint8_t *payload, size_t length)
{
    struct cn_frame_header header;
    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = message_type;
    header.payload_length = length;
    memcpy(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
    return cn_send_frame(fd, &header, payload);
}

static int send_error(int fd, const uint8_t transfer_id[16], uint32_t code, const char *msg)
{
    uint8_t payload[4 + 4 + CN_MAX_ERROR_TEXT_SIZE];
    struct cn_writer writer;
    cn_writer_init(&writer, payload, sizeof(payload));
    if (cn_writer_put_u32(&writer, code) != 0 ||
        cn_writer_put_string(&writer, msg, CN_MAX_ERROR_TEXT_SIZE) != 0) {
        return -1;
    }
    return send_frame(fd, CN_MSG_ERROR, transfer_id, payload, writer.offset);
}

static int serialize_manifest(const struct part1_manifest *manifest,
                              uint8_t **payload_out, size_t *size_out)
{
    size_t i, capacity = 4;
    uint8_t *payload;
    struct cn_writer writer;

    for (i = 0; i < manifest->count; ++i) {
        size_t name_len = strlen(manifest->files[i].name);
        if (capacity > SIZE_MAX - (4 + 4 + name_len + 8 + CN_SHA256_SIZE)) return -1;
        capacity += 4 + 4 + name_len + 8 + CN_SHA256_SIZE;
    }
    if (capacity > CN_MAX_PAYLOAD_SIZE) return -1;
    payload = malloc(capacity);
    if (payload == NULL) return -1;
    cn_writer_init(&writer, payload, capacity);
    if (cn_writer_put_u32(&writer, (uint32_t)manifest->count) != 0) {
        free(payload);
        return -1;
    }
    for (i = 0; i < manifest->count; ++i) {
        const struct part1_file *file = &manifest->files[i];
        if (cn_writer_put_u32(&writer, (uint32_t)i) != 0 ||
            cn_writer_put_string(&writer, file->name, CN_MAX_FILENAME_SIZE) != 0 ||
            cn_writer_put_u64(&writer, file->size) != 0 ||
            cn_writer_put_bytes(&writer, file->sha256, CN_SHA256_SIZE) != 0) {
            free(payload);
            return -1;
        }
    }
    *payload_out = payload;
    *size_out = writer.offset;
    return 0;
}

static void drain_pending_acks(int fd, const uint8_t transfer_id[16])
{
    for (;;) {
        struct pollfd pfd;
        struct cn_frame_header hdr;
        uint8_t *payload = NULL;
        int ret;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        ret = poll(&pfd, 1, 0);
        if (ret <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }

        if (cn_recv_frame(fd, &hdr, &payload) != CN_PROTOCOL_OK) {
            free(payload);
            break;
        }

        if (hdr.message_type == CN_MSG_ACK &&
            memcmp(hdr.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0 &&
            hdr.payload_length == 12) {
            struct cn_reader reader;
            uint32_t ack_idx;
            uint64_t ack_off;
            cn_reader_init(&reader, payload, (size_t)hdr.payload_length);
            if (cn_reader_get_u32(&reader, &ack_idx) == 0 &&
                cn_reader_get_u64(&reader, &ack_off) == 0) {
                (void)cn_session_store_update_ack(&g_session_store, transfer_id, ack_idx, ack_off);
            }
        }
        free(payload);
    }
}

static int send_file_case2(int fd, int topic_fd, const uint8_t transfer_id[16],
                          uint32_t index, const struct part1_file *file,
                          uint64_t start_offset,
                          struct cn_fault_injector *fault,
                          struct cn_serve_stats *stats)
{
    uint8_t start_payload[4 + 4 + CN_MAX_FILENAME_SIZE + 8 + CN_SHA256_SIZE];
    struct cn_writer writer;
    uint8_t buffer[DATA_CHUNK_SIZE];
    uint64_t offset = start_offset;
    int input_fd = -1, result = -1;
    struct stat st;

    cn_writer_init(&writer, start_payload, sizeof(start_payload));
    if (cn_writer_put_u32(&writer, index) != 0 ||
        cn_writer_put_string(&writer, file->name, CN_MAX_FILENAME_SIZE) != 0 ||
        cn_writer_put_u64(&writer, file->size) != 0 ||
        cn_writer_put_bytes(&writer, file->sha256, CN_SHA256_SIZE) != 0 ||
        send_frame(fd, CN_MSG_FILE_START, transfer_id, start_payload, writer.offset) != CN_PROTOCOL_OK) {
        return -1;
    }

    input_fd = openat(topic_fd, file->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input_fd < 0 || fstat(input_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != file->size) {
        goto done;
    }

    if (start_offset > 0) {
        if (lseek(input_fd, (off_t)start_offset, SEEK_SET) != (off_t)start_offset) {
            goto done;
        }
    }

    while (offset < file->size) {
        size_t want = (file->size - offset) > sizeof(buffer) ? sizeof(buffer) : (size_t)(file->size - offset);
        ssize_t got = read(input_fd, buffer, want);
        uint8_t *data_payload;
        size_t payload_size;
        struct cn_writer data_writer;
        int rc;

        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;

        drain_pending_acks(fd, transfer_id);

        payload_size = 4 + 8 + 4 + (size_t)got;
        data_payload = malloc(payload_size);
        if (data_payload == NULL) goto done;
        cn_writer_init(&data_writer, data_payload, payload_size);
        if (cn_writer_put_u32(&data_writer, index) != 0 ||
            cn_writer_put_u64(&data_writer, offset) != 0 ||
            cn_writer_put_u32(&data_writer, (uint32_t)got) != 0 ||
            cn_writer_put_bytes(&data_writer, buffer, (size_t)got) != 0) {
            free(data_payload);
            goto done;
        }

        if (fault != NULL) {
            struct cn_frame_header data_header;
            uint64_t before = fault->data_bytes_sent;
            memset(&data_header, 0, sizeof(data_header));
            data_header.version = CN_PROTOCOL_VERSION;
            data_header.message_type = CN_MSG_DATA;
            data_header.payload_length = data_writer.offset;
            memcpy(data_header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
            rc = cn_fault_send_data_frame(fd, fault, &data_header, data_payload, (size_t)got);
            if (stats != NULL) {
                stats->data_bytes_sent += fault->data_bytes_sent - before;
                stats->fault_fired = fault->fired;
            }
        } else {
            rc = send_frame(fd, CN_MSG_DATA, transfer_id, data_payload, data_writer.offset);
            if (rc == CN_PROTOCOL_OK && stats != NULL) stats->data_bytes_sent += (uint64_t)got;
        }
        free(data_payload);
        if (rc != CN_PROTOCOL_OK) goto done;
        offset += (uint64_t)got;
    }

    {
        uint8_t end_payload[4];
        struct cn_writer end_writer;
        struct cn_frame_header ack_header;
        uint8_t *ack_payload = NULL;
        struct cn_reader ack_reader;
        uint32_t ack_index;
        uint64_t ack_offset;

        cn_writer_init(&end_writer, end_payload, sizeof(end_payload));
        if (cn_writer_put_u32(&end_writer, index) != 0 ||
            send_frame(fd, CN_MSG_FILE_END, transfer_id, end_payload, end_writer.offset) != CN_PROTOCOL_OK) {
            goto done;
        }

        for (;;) {
            if (cn_recv_frame(fd, &ack_header, &ack_payload) != CN_PROTOCOL_OK) {
                free(ack_payload);
                goto done;
            }
            if (ack_header.message_type != CN_MSG_ACK ||
                memcmp(ack_header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
                free(ack_payload);
                goto done;
            }
            cn_reader_init(&ack_reader, ack_payload, (size_t)ack_header.payload_length);
            if (cn_reader_get_u32(&ack_reader, &ack_index) != 0 ||
                cn_reader_get_u64(&ack_reader, &ack_offset) != 0 ||
                cn_reader_remaining(&ack_reader) != 0 ||
                ack_index != index || ack_offset > file->size) {
                free(ack_payload);
                goto done;
            }
            free(ack_payload);
            ack_payload = NULL;

            (void)cn_session_store_update_ack(&g_session_store, transfer_id, ack_index, ack_offset);
            if (ack_offset == file->size) {
                /* Final file ACK received */
                (void)cn_session_store_update_ack(&g_session_store, transfer_id, index + 1, 0);
                break;
            }
        }
    }

    if (stats != NULL) stats->files_acked += 1;
    result = 0;

done:
    if (input_fd >= 0) close(input_fd);
    return result;
}

static int serve_client_case2(int client_fd, const char *root,
                              struct cn_fault_injector *fault,
                              struct cn_serve_stats *stats)
{
    struct timespec start;
    struct cn_io_counters counters;
    struct cn_frame_header request_header;
    uint8_t *request_payload = NULL;
    struct part1_manifest manifest;
    uint8_t *manifest_payload = NULL;
    size_t manifest_payload_len = 0;
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    uint32_t accepted_file_index = 0;
    uint64_t accepted_offset = 0;
    int root_fd = -1, topic_fd = -1, result = -1;
    char topic[CN_MAX_TOPIC_SIZE + 1] = {0};
    size_t i;

    memset(&counters, 0, sizeof(counters));
    memset(&manifest, 0, sizeof(manifest));

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
        cn_io_set_counters(&counters);
    }

    if (cn_recv_frame(client_fd, &request_header, &request_payload) != CN_PROTOCOL_OK) {
        goto done;
    }
    memcpy(transfer_id, request_header.transfer_id, CN_TRANSFER_ID_SIZE);

    if (request_header.message_type == CN_MSG_GET) {
        struct cn_reader reader;
        cn_reader_init(&reader, request_payload, (size_t)request_header.payload_length);
        if (cn_reader_get_string(&reader, topic, sizeof(topic), CN_MAX_TOPIC_SIZE) != 0 ||
            cn_reader_remaining(&reader) != 0 || !part1_validate_topic_name(topic)) {
            send_error(client_fd, transfer_id, CN_ERR_INVALID_REQUEST, "invalid GET topic name");
            goto done;
        }
        if (stats != NULL) (void)snprintf(stats->topic, sizeof(stats->topic), "%s", topic);

        if (part1_build_manifest(root, topic, &manifest) != 0) {
            send_error(client_fd, transfer_id, CN_ERR_UNKNOWN_TOPIC, "topic unavailable or could not be read");
            goto done;
        }

        if (serialize_manifest(&manifest, &manifest_payload, &manifest_payload_len) != 0 ||
            cn_compute_manifest_identity(manifest_payload, manifest_payload_len, manifest_identity) != 0) {
            send_error(client_fd, transfer_id, CN_ERR_INTERNAL, "failed to serialize manifest");
            goto done;
        }

        (void)cn_session_store_create(&g_session_store, transfer_id, topic, manifest_identity);

        if (send_frame(client_fd, CN_MSG_MANIFEST, transfer_id, manifest_payload, manifest_payload_len) != CN_PROTOCOL_OK) {
            goto done;
        }

        if (stats != NULL) {
            stats->file_count = (uint32_t)manifest.count;
            stats->data_bytes_planned = manifest.total_bytes;
        }
        if (fault != NULL) {
            (void)cn_fault_plan(fault, manifest.total_bytes);
        }
        accepted_file_index = 0;
        accepted_offset = 0;

    } else if (request_header.message_type == CN_MSG_RESUME) {
        struct cn_resume_request req;
        char reason[256];
        uint8_t current_identity[CN_SHA256_SIZE];
        uint8_t *resume_ok_payload = NULL;
        size_t resume_ok_len = 0;
        uint64_t remaining_bytes = 0;

        if (cn_decode_resume(request_payload, (size_t)request_header.payload_length, &req) != 0) {
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "malformed RESUME payload");
            goto done;
        }
        if (memcmp(transfer_id, req.transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "header and payload transfer ID differ");
            goto done;
        }
        (void)snprintf(topic, sizeof(topic), "%s", req.topic);
        if (stats != NULL) (void)snprintf(stats->topic, sizeof(stats->topic), "%s", topic);

        if (!part1_validate_topic_name(req.topic)) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "invalid topic name in RESUME");
            goto done;
        }

        if (part1_build_manifest(root, req.topic, &manifest) != 0) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "topic cannot be read");
            goto done;
        }

        if (serialize_manifest(&manifest, &manifest_payload, &manifest_payload_len) != 0 ||
            cn_compute_manifest_identity(manifest_payload, manifest_payload_len, current_identity) != 0) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_INTERNAL, "failed to compute manifest identity");
            goto done;
        }

        if (memcmp(req.manifest_identity, current_identity, CN_SHA256_SIZE) != 0) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "topic contents have changed on server");
            goto done;
        }

        if (cn_session_store_validate_resume(&g_session_store, transfer_id, req.topic,
                                             req.manifest_identity, req.file_index,
                                             req.contiguous_offset, &accepted_file_index,
                                             &accepted_offset, reason, sizeof(reason)) != 0) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, reason);
            goto done;
        }

        /* Validate completed files list against current manifest */
        for (i = 0; i < req.completed_count; ++i) {
            uint32_t c_idx = req.completed[i].file_index;
            if (c_idx >= manifest.count ||
                memcmp(req.completed[i].sha256, manifest.files[c_idx].sha256, CN_SHA256_SIZE) != 0) {
                cn_resume_request_free(&req);
                send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "completed file hash mismatch");
                goto done;
            }
        }

        /* Validate accepted offset against file size */
        if (accepted_file_index > manifest.count ||
            (accepted_file_index < manifest.count && accepted_offset > manifest.files[accepted_file_index].size)) {
            cn_resume_request_free(&req);
            send_error(client_fd, transfer_id, CN_ERR_RESUME_REJECTED, "accepted offset out of bounds");
            goto done;
        }

        cn_resume_request_free(&req);

        if (cn_encode_resume_ok(accepted_file_index, accepted_offset, manifest_payload,
                                manifest_payload_len, &resume_ok_payload, &resume_ok_len) != 0 ||
            send_frame(client_fd, CN_MSG_RESUME_OK, transfer_id, resume_ok_payload, resume_ok_len) != CN_PROTOCOL_OK) {
            free(resume_ok_payload);
            goto done;
        }
        free(resume_ok_payload);

        /* Calculate remaining bytes to plan fault injection on subsequent bytes only */
        if (accepted_file_index < manifest.count) {
            remaining_bytes = manifest.files[accepted_file_index].size - accepted_offset;
            for (i = accepted_file_index + 1; i < manifest.count; ++i) {
                remaining_bytes += manifest.files[i].size;
            }
        }

        if (stats != NULL) {
            stats->file_count = (uint32_t)manifest.count;
            stats->data_bytes_planned = remaining_bytes;
        }
        if (fault != NULL) {
            (void)cn_fault_plan(fault, remaining_bytes);
        }

    } else {
        send_error(client_fd, transfer_id, CN_ERR_PROTOCOL, "expected GET or RESUME");
        goto done;
    }

    root_fd = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (root_fd < 0) goto done;
    topic_fd = openat(root_fd, topic, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (topic_fd < 0) goto done;

    for (i = accepted_file_index; i < manifest.count; ++i) {
        uint64_t start_off = (i == accepted_file_index) ? accepted_offset : 0;
        if (send_file_case2(client_fd, topic_fd, transfer_id, (uint32_t)i,
                            &manifest.files[i], start_off, fault, stats) != 0) {
            goto done;
        }
    }

    {
        uint8_t complete_payload[4 + 8];
        struct cn_writer complete_writer;
        cn_writer_init(&complete_writer, complete_payload, sizeof(complete_payload));
        if (cn_writer_put_u32(&complete_writer, (uint32_t)manifest.count) != 0 ||
            cn_writer_put_u64(&complete_writer, manifest.total_bytes) != 0 ||
            send_frame(client_fd, CN_MSG_COMPLETE, transfer_id, complete_payload,
                       complete_writer.offset) != CN_PROTOCOL_OK) {
            goto done;
        }
    }

    cn_session_store_remove(&g_session_store, transfer_id);
    if (stats != NULL) stats->completed = 1;
    result = 0;

done:
    if (stats != NULL) {
        if (stats->topic[0] != '\0') stats->seconds = elapsed_since(&start);
        stats->io = counters;
        if (fault != NULL) stats->fault_fired = fault->fired;
        cn_io_set_counters(NULL);
    }
    free(request_payload);
    free(manifest_payload);
    if (topic_fd >= 0) close(topic_fd);
    if (root_fd >= 0) close(root_fd);
    part1_free_manifest(&manifest);
    return result;
}

static void *serve_worker(void *opaque)
{
    struct worker_args *args = opaque;
    struct cn_fault_injector fault;
    struct cn_serve_stats stats;
    struct timeval tv;

    cn_fault_injector_init(&fault, args->fault_config, args->connection_id);
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    (void)setsockopt(args->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(args->client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    (void)cn_socket_set_nodelay(args->client_fd);

    (void)serve_client_case2(args->client_fd, args->root, &fault, &stats);
    record_connection(args, &fault, &stats);

    close(args->client_fd);
    free(args);
    return NULL;
}

int main(int argc, char **argv)
{
    uint16_t port;
    double probability;
    struct cn_fault_config fault_config;
    const char *error = NULL;
    DIR *root_dir;
    int server_fd, reuse = 1;
    struct sockaddr_in address;
    uint64_t next_connection_id = 1;
    const char *csv_path;

    if (argc != 4 || cn_parse_port(argv[1], &port) != 0 ||
        cn_parse_probability(argv[3], &probability) != 0) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability>\n", argv[0]);
        return EXIT_FAILURE;
    }

    root_dir = opendir(argv[2]);
    if (root_dir == NULL) {
        fprintf(stderr, "%s: cannot open topics root %s: %s\n", argv[0], argv[2], strerror(errno));
        return EXIT_FAILURE;
    }
    closedir(root_dir);

    if (cn_fault_config_init(&fault_config, probability, &error) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], error);
        return EXIT_FAILURE;
    }

    csv_path = getenv("CN_SERVER_METRICS_CSV");
    if (csv_path != NULL && csv_path[0] != '\0') {
        metrics_csv = fopen(csv_path, "a");
        if (metrics_csv == NULL) {
            fprintf(stderr, "server: cannot open metrics CSV %s: %s\n", csv_path, strerror(errno));
            return EXIT_FAILURE;
        }
        if (ftell(metrics_csv) == 0) {
            fprintf(metrics_csv,
                    "connection_id,topic,outcome,probability,fault_mode,files_acked,"
                    "files_total,data_bytes_sent,data_bytes_planned,wire_bytes_sent,"
                    "wire_bytes_received,duration_seconds,fault_fired,cut_at_data_byte\n");
            fflush(metrics_csv);
        }
    }

    if (cn_session_store_init(&g_session_store, CN_DEFAULT_SESSION_TIMEOUT_SEC) != 0) {
        fprintf(stderr, "server: failed to initialize session store\n");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_fd, 128) != 0) {
        perror("bind/listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Part 2 Case 2 server listening on port %u (failure probability %.4f, "
           "mode %s, action %s, seed %" PRIu64 "%s)\n",
           (unsigned)port, fault_config.probability, cn_fault_mode_name(fault_config.mode),
           cn_fault_action_name(fault_config.action), fault_config.seed,
           fault_config.seed_from_env ? "" : " from clock");
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        pthread_t thread;
        struct worker_args *args;

        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        args = malloc(sizeof(*args));
        if (args == NULL) {
            close(client_fd);
            continue;
        }
        args->client_fd = client_fd;
        args->connection_id = next_connection_id++;
        args->root = argv[2];
        args->fault_config = &fault_config;

        if (pthread_create(&thread, NULL, serve_worker, args) != 0) {
            close(client_fd);
            free(args);
            continue;
        }
        (void)pthread_detach(thread);
    }
}
