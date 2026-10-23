#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 2 client: durable checkpointing and resumption.
 *
 * Usage: client <server_ip> <server_port> <topic> <output_dir>
 *
 * Each attempt checks for a durable checkpoint under <output_dir>/<topic>/.checkpoint.
 * If one exists, the client truncates any uncheckpointed trailing bytes from
 * the active .part file and sends RESUME with the checkpointed state (transfer ID,
 * topic, manifest identity, file index, contiguous offset, completed file hashes).
 *
 * If the server accepts (RESUME_OK), transfer continues from the accepted file/offset
 * with zero duplicate retransmission of completed files or acknowledged data.
 * If the server rejects the resume (missing session, restart, expiration), the client
 * safely falls back to a fresh GET from the beginning with a new transfer ID.
 *
 * On exit the client prints one METRICS line.
 */

#include "checkpoint.h"
#include "cli.h"
#include "client.h"
#include "../common/case2_protocol.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_SECONDS 30u
#define CONNECT_GIVEUP_SECONDS 30u
#define CONNECT_BACKOFF_MAX_MS 2000u
#define DEFAULT_CHECKPOINT_INTERVAL (64u * 1024u)

struct run_metrics {
    uint32_t attempts;
    uint32_t files;
    uint64_t wire_bytes_received;   /* every byte from server across all attempts */
    uint64_t wire_bytes_sent;       /* frames sent to server across all attempts */
    uint64_t data_bytes_received;   /* file data bytes in complete DATA frames */
    uint64_t useful_bytes;          /* delivered and verified topic file bytes */
    uint64_t final_wire_bytes;      /* bytes received by the final attempt */
    double seconds;
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

static uint32_t env_u32(const char *name, uint32_t fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return fallback;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

static int is_retryable(enum cn_client_status status)
{
    return status == CN_CLIENT_NETWORK || status == CN_CLIENT_PROTOCOL ||
           status == CN_CLIENT_INTEGRITY;
}

static double ratio(double numerator, double denominator)
{
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

static void print_metrics(FILE *out, const struct run_metrics *m, enum cn_client_status status)
{
    uint64_t redundant = m->data_bytes_received - m->useful_bytes;

    fprintf(out,
            "METRICS status=%d attempts=%" PRIu32 " files=%" PRIu32
            " transmitted_bytes=%" PRIu64 " sent_bytes=%" PRIu64
            " data_bytes_received=%" PRIu64 " useful_bytes=%" PRIu64
            " redundant_bytes=%" PRIu64 " wasted_bytes=%" PRIu64
            " overhead=%.4f completion_s=%.6f goodput_MBps=%.3f throughput_MBps=%.3f\n",
            (int)status, m->attempts, m->files, m->wire_bytes_received, m->wire_bytes_sent,
            m->data_bytes_received, m->useful_bytes, redundant,
            m->wire_bytes_received - m->final_wire_bytes,
            ratio((double)(m->wire_bytes_received - m->useful_bytes), (double)m->useful_bytes),
            m->seconds, ratio((double)m->useful_bytes, m->seconds) / 1e6,
            ratio((double)m->wire_bytes_received, m->seconds) / 1e6);
}

static void append_csv(const char *topic, const struct run_metrics *m,
                       enum cn_client_status status)
{
    const char *path = getenv("CN_METRICS_CSV");
    struct stat st;
    FILE *csv;
    int is_new;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    is_new = stat(path, &st) != 0 || st.st_size == 0;
    csv = fopen(path, "a");
    if (csv == NULL) {
        fprintf(stderr, "client: cannot append metrics to %s: %s\n", path, strerror(errno));
        return;
    }
    if (is_new) {
        fprintf(csv, "topic,status,attempts,files,transmitted_bytes,sent_bytes,"
                     "data_bytes_received,useful_bytes,redundant_bytes,wasted_bytes,"
                     "overhead,completion_s,goodput_MBps,throughput_MBps\n");
    }
    fprintf(csv, "%s,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f\n",
            topic, (int)status, m->attempts, m->files, m->wire_bytes_received,
            m->wire_bytes_sent, m->data_bytes_received, m->useful_bytes,
            m->data_bytes_received - m->useful_bytes,
            m->wire_bytes_received - m->final_wire_bytes,
            ratio((double)(m->wire_bytes_received - m->useful_bytes), (double)m->useful_bytes),
            m->seconds, ratio((double)m->useful_bytes, m->seconds) / 1e6,
            ratio((double)m->wire_bytes_received, m->seconds) / 1e6);
    fclose(csv);
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

static int send_ack(int fd, const uint8_t transfer_id[16], uint32_t file_index,
                    uint64_t contiguous_offset)
{
    uint8_t payload[4 + 8];
    struct cn_writer writer;
    cn_writer_init(&writer, payload, sizeof(payload));
    if (cn_writer_put_u32(&writer, file_index) != 0 ||
        cn_writer_put_u64(&writer, contiguous_offset) != 0) {
        return -1;
    }
    return send_frame(fd, CN_MSG_ACK, transfer_id, payload, writer.offset);
}

static void clean_topic_parts(const char *output_dir, const char *topic)
{
    char topic_dir[1024];
    DIR *d;
    struct dirent *de;

    (void)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", output_dir, topic);
    d = opendir(topic_dir);
    if (d == NULL) return;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 5 && strcmp(de->d_name + len - 5, ".part") == 0) {
            char path[2048];
            (void)snprintf(path, sizeof(path), "%s/%s", topic_dir, de->d_name);
            (void)unlink(path);
        }
    }
    closedir(d);
}

static void truncate_any_part_files(const char *output_dir, const char *topic,
                                    uint64_t offset)
{
    char topic_dir[1024];
    DIR *d;
    struct dirent *de;

    (void)snprintf(topic_dir, sizeof(topic_dir), "%s/%s", output_dir, topic);
    d = opendir(topic_dir);
    if (d == NULL) return;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len > 5 && strcmp(de->d_name + len - 5, ".part") == 0) {
            char path[2048];
            struct stat st;
            (void)snprintf(path, sizeof(path), "%s/%s", topic_dir, de->d_name);
            if (stat(path, &st) == 0 && (uint64_t)st.st_size > offset) {
                int fd = open(path, O_RDWR | O_CLOEXEC);
                if (fd >= 0) {
                    if (ftruncate(fd, (off_t)offset) == 0) {
                        (void)fsync(fd);
                    }
                    close(fd);
                }
            }
        }
    }
    closedir(d);
}

/* Forgets the checkpointed bytes of one file so the next RESUME restarts it at offset 0. */
static void restart_file(const char *output_dir, const char *topic,
                         struct cn_checkpoint *ckpt, uint32_t file_index)
{
    ckpt->file_index = file_index;
    ckpt->contiguous_offset = 0;
    (void)cn_checkpoint_save(output_dir, topic, ckpt);
}

static enum cn_client_status receive_topic_files(
    int socket_fd, const uint8_t transfer_id[16], const char *output_dir,
    const char *topic, struct cn_manifest *manifest, uint32_t start_index,
    uint64_t start_offset, uint32_t ckpt_threshold, struct cn_checkpoint *ckpt,
    FILE *log, struct cn_client_report *report)
{
    int dir_fd = -1;
    uint32_t i;
    enum cn_client_status status = CN_CLIENT_OK;

    dir_fd = cn_client_open_topic_dir(output_dir, topic);
    if (dir_fd < 0) {
        report->status = CN_CLIENT_LOCAL_IO;
        (void)snprintf(report->message, sizeof(report->message), "cannot open topic directory %s/%s",
                       output_dir, topic);
        return CN_CLIENT_LOCAL_IO;
    }

    for (i = start_index; i < manifest->count; ++i) {
        const struct cn_manifest_entry *entry = &manifest->entries[i];
        uint64_t file_start_offset = (i == start_index) ? start_offset : 0;
        struct cn_file_sink sink = CN_FILE_SINK_INIT;
        struct cn_frame_header header;
        uint8_t *payload = NULL;
        uint64_t unacked_bytes = 0;

        /* Expect FILE_START */
        if (cn_recv_frame(socket_fd, &header, &payload) != CN_PROTOCOL_OK) {
            free(payload);
            status = CN_CLIENT_NETWORK;
            (void)snprintf(report->message, sizeof(report->message), "server disconnected before FILE_START");
            goto done;
        }
        if (memcmp(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) != 0 ||
            header.message_type != CN_MSG_FILE_START) {
            free(payload);
            status = CN_CLIENT_PROTOCOL;
            (void)snprintf(report->message, sizeof(report->message), "expected FILE_START, got %u", header.message_type);
            goto done;
        }
        free(payload);
        payload = NULL;

        if (file_start_offset > 0) {
            if (cn_file_sink_open_resume(&sink, dir_fd, entry, file_start_offset) != 0) {
                /* The partial file is missing or shorter than the resume point, for
                   example because the file was already renamed into place before the
                   server stored its final ACK. Retry with this file from the start. */
                restart_file(output_dir, topic, ckpt, entry->file_index);
                status = CN_CLIENT_INTEGRITY;
                (void)snprintf(report->message, sizeof(report->message),
                               "no partial data to resume %s; restarting the file", entry->name);
                goto done;
            }
        } else {
            if (cn_file_sink_open(&sink, dir_fd, entry) != 0) {
                status = CN_CLIENT_LOCAL_IO;
                (void)snprintf(report->message, sizeof(report->message), "cannot open .part file");
                goto done;
            }
        }

        /* Receive DATA frames until FILE_END. On failure the partial file is kept:
           the next attempt truncates it to the checkpoint and resumes from there. */
        for (;;) {
            if (cn_recv_frame(socket_fd, &header, &payload) != CN_PROTOCOL_OK) {
                free(payload);
                status = CN_CLIENT_NETWORK;
                (void)snprintf(report->message, sizeof(report->message), "server disconnected in DATA");
                cn_file_sink_close(&sink);
                goto done;
            }

            if (memcmp(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
                free(payload);
                status = CN_CLIENT_PROTOCOL;
                (void)snprintf(report->message, sizeof(report->message), "mismatched transfer ID in data frame");
                cn_file_sink_close(&sink);
                goto done;
            }

            if (header.message_type == CN_MSG_DATA) {
                struct cn_reader reader;
                uint32_t data_index, data_len;
                uint64_t data_offset;
                cn_reader_init(&reader, payload, (size_t)header.payload_length);
                if (cn_reader_get_u32(&reader, &data_index) != 0 ||
                    cn_reader_get_u64(&reader, &data_offset) != 0 ||
                    cn_reader_get_u32(&reader, &data_len) != 0 ||
                    cn_reader_remaining(&reader) != data_len ||
                    data_index != entry->file_index) {
                    free(payload);
                    status = CN_CLIENT_PROTOCOL;
                    (void)snprintf(report->message, sizeof(report->message), "malformed DATA frame");
                    cn_file_sink_close(&sink);
                    goto done;
                }
                /* The checkpoint records a contiguous prefix, so DATA must not skip ahead. */
                if (data_offset != sink.received) {
                    free(payload);
                    status = CN_CLIENT_PROTOCOL;
                    (void)snprintf(report->message, sizeof(report->message),
                                   "DATA for %s at offset %" PRIu64 ", expected %" PRIu64,
                                   entry->name, data_offset, sink.received);
                    cn_file_sink_close(&sink);
                    goto done;
                }

                if (cn_file_sink_write(&sink, data_offset, payload + reader.offset, data_len) != 0) {
                    free(payload);
                    status = CN_CLIENT_LOCAL_IO;
                    (void)snprintf(report->message, sizeof(report->message), "failed to write to .part file");
                    cn_file_sink_close(&sink);
                    goto done;
                }
                free(payload);
                payload = NULL;

                report->data_bytes_received += data_len;
                unacked_bytes += data_len;

                /* Durable checkpoint at configured chunk boundary; ACK only what is
                   flushed and checkpointed, so the server never gets ahead of us. */
                if (unacked_bytes >= ckpt_threshold && data_offset + data_len < entry->size) {
                    ckpt->file_index = entry->file_index;
                    ckpt->contiguous_offset = data_offset + data_len;
                    if (fsync(sink.fd) == 0 && cn_checkpoint_save(output_dir, topic, ckpt) == 0) {
                        (void)send_ack(socket_fd, transfer_id, entry->file_index, data_offset + data_len);
                    }
                    unacked_bytes = 0;
                }

            } else if (header.message_type == CN_MSG_FILE_END) {
                struct cn_reader reader;
                uint32_t end_index;
                cn_reader_init(&reader, payload, (size_t)header.payload_length);
                if (cn_reader_get_u32(&reader, &end_index) != 0 ||
                    cn_reader_remaining(&reader) != 0 || end_index != entry->file_index) {
                    free(payload);
                    status = CN_CLIENT_PROTOCOL;
                    (void)snprintf(report->message, sizeof(report->message), "malformed FILE_END frame");
                    cn_file_sink_close(&sink);
                    goto done;
                }
                free(payload);
                payload = NULL;

                /* Commit file: flushes, verifies size & SHA-256, renames atomically */
                status = cn_file_sink_commit(&sink);
                if (status != CN_CLIENT_OK) {
                    (void)snprintf(report->message, sizeof(report->message), "file integrity check failed for %s", entry->name);
                    /* The partial data is bad, so its checkpoint is worthless too. */
                    cn_file_sink_abort(&sink);
                    restart_file(output_dir, topic, ckpt, entry->file_index);
                    goto done;
                }

                /* File completely verified! Record in checkpoint */
                (void)cn_checkpoint_add_completed(ckpt, entry->file_index, entry->sha256);
                ckpt->file_index = entry->file_index + 1;
                ckpt->contiguous_offset = 0;
                (void)cn_checkpoint_save(output_dir, topic, ckpt);

                /* Send file-level ACK */
                if (send_ack(socket_fd, transfer_id, entry->file_index, entry->size) != CN_PROTOCOL_OK) {
                    status = CN_CLIENT_NETWORK;
                    (void)snprintf(report->message, sizeof(report->message), "failed to send ACK for %s", entry->name);
                    goto done;
                }

                report->files_completed += 1;
                report->bytes_completed += entry->size;
                if (log != NULL) {
                    fprintf(log, "Received %s (%" PRIu64 " bytes)\n", entry->name, entry->size);
                    fflush(log);
                }
                break;
            } else {
                free(payload);
                status = CN_CLIENT_PROTOCOL;
                (void)snprintf(report->message, sizeof(report->message), "unexpected frame type %u", header.message_type);
                cn_file_sink_close(&sink);
                goto done;
            }
        }
    }

    /* Expect COMPLETE */
    {
        struct cn_frame_header header;
        uint8_t *payload = NULL;
        struct cn_reader reader;
        uint32_t file_count;
        uint64_t total_bytes;

        if (cn_recv_frame(socket_fd, &header, &payload) != CN_PROTOCOL_OK) {
            free(payload);
            status = CN_CLIENT_NETWORK;
            (void)snprintf(report->message, sizeof(report->message), "server disconnected before COMPLETE");
            goto done;
        }
        if (header.message_type != CN_MSG_COMPLETE) {
            free(payload);
            status = CN_CLIENT_PROTOCOL;
            (void)snprintf(report->message, sizeof(report->message), "expected COMPLETE frame");
            goto done;
        }
        cn_reader_init(&reader, payload, (size_t)header.payload_length);
        if (cn_reader_get_u32(&reader, &file_count) != 0 ||
            cn_reader_get_u64(&reader, &total_bytes) != 0 ||
            cn_reader_remaining(&reader) != 0 ||
            file_count != manifest->count || total_bytes != manifest->total_bytes) {
            free(payload);
            status = CN_CLIENT_PROTOCOL;
            (void)snprintf(report->message, sizeof(report->message), "inconsistent COMPLETE frame");
            goto done;
        }
        free(payload);
    }

    /* All files verified and COMPLETE confirmed: remove checkpoint */
    cn_checkpoint_remove(output_dir, topic);
    report->files_completed = manifest->count;
    report->bytes_completed = manifest->total_bytes;
    status = CN_CLIENT_OK;

done:
    if (dir_fd >= 0) close(dir_fd);
    return status;
}

int main(int argc, char **argv)
{
    struct cn_client_report report;
    struct run_metrics metrics;
    struct timespec start;
    enum cn_client_status status = CN_CLIENT_NETWORK;
    uint32_t max_attempts, retry_delay_ms, timeout_seconds, connect_giveup_seconds;
    uint32_t ckpt_threshold;
    struct timespec first_connect_failure;
    int connect_failing = 0;
    unsigned backoff_ms;
    uint16_t port;
    FILE *log;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    struct cn_checkpoint ckpt;
    int has_checkpoint = 0;

    if (argc != 5 || cn_parse_port(argv[2], &port) != 0 || argv[4][0] == '\0') {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir>\n", argv[0]);
        return CN_CLIENT_USAGE;
    }
    if (!cn_client_name_is_valid(argv[3], CN_MAX_TOPIC_SIZE)) {
        fprintf(stderr, "client: invalid topic name: use 1-%u bytes without '/', '\\\\' or "
                        "control characters, and not '.' or '..'\\n", CN_MAX_TOPIC_SIZE);
        return CN_CLIENT_USAGE;
    }

    signal(SIGPIPE, SIG_IGN);
    max_attempts = env_u32("CN_MAX_ATTEMPTS", 100u);
    retry_delay_ms = env_u32("CN_RETRY_DELAY_MS", 50u);
    timeout_seconds = env_u32("CN_TIMEOUT_SECONDS", TIMEOUT_SECONDS);
    connect_giveup_seconds = env_u32("CN_CONNECT_GIVEUP_SECONDS", CONNECT_GIVEUP_SECONDS);
    ckpt_threshold = env_u32("CN_CHECKPOINT_BYTES", DEFAULT_CHECKPOINT_INTERVAL);
    if (timeout_seconds == 0) timeout_seconds = TIMEOUT_SECONDS;

    log = env_u32("CN_QUIET", 0u) != 0 ? NULL : stdout;
    memset(&metrics, 0, sizeof(metrics));
    memset(&report, 0, sizeof(report));
    memset(&ckpt, 0, sizeof(ckpt));

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    /* Check if an existing checkpoint is available for this topic */
    if (cn_checkpoint_load(argv[4], argv[3], &ckpt) == 0 &&
        strcmp(ckpt.topic, argv[3]) == 0) {
        memcpy(transfer_id, ckpt.transfer_id, CN_TRANSFER_ID_SIZE);
        has_checkpoint = 1;
        if (log != NULL) {
            fprintf(log, "Found existing checkpoint for %s: file %" PRIu32 " offset %" PRIu64 "\n",
                    argv[3], ckpt.file_index, ckpt.contiguous_offset);
        }
    } else {
        if (cn_client_new_transfer_id(transfer_id) != 0) {
            perror("client: cannot generate a transfer ID");
            return CN_CLIENT_LOCAL_IO;
        }
    }

    backoff_ms = retry_delay_ms > 0 ? retry_delay_ms : 1u;

    while (metrics.attempts < max_attempts || max_attempts == 0) {
        int socket_fd;
        struct cn_io_counters io = {0, 0};
        metrics.attempts += 1;
        memset(&report, 0, sizeof(report));

        socket_fd = cn_client_connect(argv[1], port, timeout_seconds, &report);
        if (socket_fd < 0) {
            if (!connect_failing) {
                connect_failing = 1;
                (void)clock_gettime(CLOCK_MONOTONIC, &first_connect_failure);
            }
            if (seconds_since(&first_connect_failure) >= (double)connect_giveup_seconds) {
                fprintf(stderr, "client: server unreachable for %" PRIu32 " s, giving up\n",
                        connect_giveup_seconds);
                status = CN_CLIENT_NETWORK;
                break;
            }
            sleep_ms(backoff_ms);
            backoff_ms = backoff_ms * 2u > CONNECT_BACKOFF_MAX_MS ? CONNECT_BACKOFF_MAX_MS : backoff_ms * 2u;
            continue;
        }
        connect_failing = 0;
        backoff_ms = retry_delay_ms > 0 ? retry_delay_ms : 1u;

        cn_io_set_counters(&io);

        if (has_checkpoint) {
            /* Truncate uncheckpointed bytes from partial files before sending RESUME */
            truncate_any_part_files(argv[4], argv[3], ckpt.contiguous_offset);

            struct cn_resume_request req;
            uint8_t *resume_payload = NULL;
            size_t resume_len = 0;
            struct cn_frame_header resp_hdr;
            uint8_t *resp_payload = NULL;

            memset(&req, 0, sizeof(req));
            memcpy(req.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
            (void)snprintf(req.topic, sizeof(req.topic), "%s", argv[3]);
            memcpy(req.manifest_identity, ckpt.manifest_identity, CN_SHA256_SIZE);
            req.file_index = ckpt.file_index;
            req.contiguous_offset = ckpt.contiguous_offset;
            req.completed_count = ckpt.completed_count;
            req.completed = ckpt.completed;

            if (cn_encode_resume(&req, &resume_payload, &resume_len) != 0 ||
                send_frame(socket_fd, CN_MSG_RESUME, transfer_id, resume_payload, resume_len) != CN_PROTOCOL_OK) {
                free(resume_payload);
                close(socket_fd);
                status = CN_CLIENT_NETWORK;
                goto attempt_done;
            }
            free(resume_payload);

            if (cn_recv_frame(socket_fd, &resp_hdr, &resp_payload) != CN_PROTOCOL_OK) {
                free(resp_payload);
                close(socket_fd);
                status = CN_CLIENT_NETWORK;
                goto attempt_done;
            }

            if (resp_hdr.message_type == CN_MSG_ERROR) {
                struct cn_reader reader;
                uint32_t code = 0;
                char err_text[CN_MAX_ERROR_TEXT_SIZE + 1] = {0};
                cn_reader_init(&reader, resp_payload, (size_t)resp_hdr.payload_length);
                (void)cn_reader_get_u32(&reader, &code);
                (void)cn_reader_get_string(&reader, err_text, sizeof(err_text), CN_MAX_ERROR_TEXT_SIZE);
                free(resp_payload);
                close(socket_fd);

                if (code == CN_ERR_RESUME_REJECTED) {
                    /* Server rejected resume: safe fallback to fresh GET */
                    fprintf(stderr, "client: RESUME rejected by server (%s); falling back to GET\n",
                            err_text);
                    cn_checkpoint_remove(argv[4], argv[3]);
                    clean_topic_parts(argv[4], argv[3]);
                    cn_checkpoint_free(&ckpt);
                    has_checkpoint = 0;
                    (void)cn_client_new_transfer_id(transfer_id);
                    status = CN_CLIENT_NETWORK;
                    (void)snprintf(report.message, sizeof(report.message),
                                   "RESUME rejected; restarting with GET");
                    goto attempt_done;
                } else {
                    status = CN_CLIENT_SERVER_ERROR;
                    (void)snprintf(report.message, sizeof(report.message),
                                   "server error: %.497s", err_text);
                    goto attempt_done;
                }
            }

            if (resp_hdr.message_type == CN_MSG_RESUME_OK) {
                uint32_t acc_file_idx = 0;
                uint64_t acc_offset = 0;
                const uint8_t *manifest_data = NULL;
                size_t manifest_len = 0;
                struct cn_manifest manifest = {NULL, 0, 0};
                const char *reason = NULL;
                uint8_t identity[CN_SHA256_SIZE];

                /* The server may resume earlier than our checkpoint (it never saw
                   the last ACKs) but never later, and only for the same manifest. */
                if (cn_decode_resume_ok(resp_payload, (size_t)resp_hdr.payload_length,
                                        &acc_file_idx, &acc_offset, &manifest_data, &manifest_len) != 0 ||
                    cn_compute_manifest_identity(manifest_data, manifest_len, identity) != 0 ||
                    memcmp(identity, ckpt.manifest_identity, CN_SHA256_SIZE) != 0 ||
                    acc_file_idx > ckpt.file_index ||
                    (acc_file_idx == ckpt.file_index && acc_offset > ckpt.contiguous_offset) ||
                    cn_manifest_parse(manifest_data, manifest_len, &manifest, &reason) != 0) {
                    free(resp_payload);
                    close(socket_fd);
                    status = CN_CLIENT_PROTOCOL;
                    (void)snprintf(report.message, sizeof(report.message), "invalid RESUME_OK");
                    goto attempt_done;
                }
                free(resp_payload);

                /* Never leave a checkpoint that claims more than the server accepted. */
                if (acc_file_idx != ckpt.file_index || acc_offset != ckpt.contiguous_offset) {
                    ckpt.file_index = acc_file_idx;
                    ckpt.contiguous_offset = acc_offset;
                    (void)cn_checkpoint_save(argv[4], argv[3], &ckpt);
                }

                if (log != NULL) {
                    fprintf(log, "Resuming topic %s from file %" PRIu32 " offset %" PRIu64 "\n",
                            argv[3], acc_file_idx, acc_offset);
                }

                status = receive_topic_files(socket_fd, transfer_id, argv[4], argv[3],
                                             &manifest, acc_file_idx, acc_offset,
                                             ckpt_threshold, &ckpt, log, &report);
                cn_manifest_free(&manifest);
                close(socket_fd);
            } else {
                free(resp_payload);
                close(socket_fd);
                status = CN_CLIENT_PROTOCOL;
                goto attempt_done;
            }

        } else {
            /* Fresh GET transfer */
            uint8_t get_payload[4 + CN_MAX_TOPIC_SIZE];
            struct cn_writer writer;
            struct cn_frame_header resp_hdr;
            uint8_t *resp_payload = NULL;
            struct cn_manifest manifest = {NULL, 0, 0};
            const char *reason = NULL;
            uint8_t manifest_identity[CN_SHA256_SIZE];

            cn_writer_init(&writer, get_payload, sizeof(get_payload));
            if (cn_writer_put_string(&writer, argv[3], CN_MAX_TOPIC_SIZE) != 0 ||
                send_frame(socket_fd, CN_MSG_GET, transfer_id, get_payload, writer.offset) != CN_PROTOCOL_OK) {
                close(socket_fd);
                status = CN_CLIENT_NETWORK;
                goto attempt_done;
            }

            if (cn_recv_frame(socket_fd, &resp_hdr, &resp_payload) != CN_PROTOCOL_OK) {
                free(resp_payload);
                close(socket_fd);
                status = CN_CLIENT_NETWORK;
                goto attempt_done;
            }

            if (resp_hdr.message_type == CN_MSG_ERROR) {
                struct cn_reader reader;
                uint32_t code = 0;
                char err_text[CN_MAX_ERROR_TEXT_SIZE + 1] = {0};
                cn_reader_init(&reader, resp_payload, (size_t)resp_hdr.payload_length);
                (void)cn_reader_get_u32(&reader, &code);
                (void)cn_reader_get_string(&reader, err_text, sizeof(err_text), CN_MAX_ERROR_TEXT_SIZE);
                free(resp_payload);
                close(socket_fd);
                status = CN_CLIENT_SERVER_ERROR;
                (void)snprintf(report.message, sizeof(report.message),
                               "server error: %.497s", err_text);
                goto attempt_done;
            }

            if (resp_hdr.message_type != CN_MSG_MANIFEST ||
                cn_manifest_parse(resp_payload, (size_t)resp_hdr.payload_length, &manifest, &reason) != 0 ||
                cn_compute_manifest_identity(resp_payload, (size_t)resp_hdr.payload_length, manifest_identity) != 0) {
                free(resp_payload);
                close(socket_fd);
                status = CN_CLIENT_PROTOCOL;
                goto attempt_done;
            }
            free(resp_payload);

            /* Initialize durable checkpoint */
            (void)cn_checkpoint_init(&ckpt, transfer_id, argv[3], manifest_identity);
            (void)cn_checkpoint_save(argv[4], argv[3], &ckpt);
            has_checkpoint = 1;

            if (log != NULL) {
                fprintf(log, "Topic %s: %" PRIu32 " file(s), %" PRIu64 " byte(s)\n",
                        argv[3], manifest.count, manifest.total_bytes);
            }

            status = receive_topic_files(socket_fd, transfer_id, argv[4], argv[3],
                                         &manifest, 0, 0, ckpt_threshold, &ckpt,
                                         log, &report);
            cn_manifest_free(&manifest);
            close(socket_fd);
        }

attempt_done:
        cn_io_set_counters(NULL);
        metrics.wire_bytes_received += io.bytes_received;
        metrics.wire_bytes_sent += io.bytes_sent;
        metrics.data_bytes_received += report.data_bytes_received;

        if (status == CN_CLIENT_OK) {
            metrics.useful_bytes = report.bytes_completed;
            metrics.files = report.files_completed;
            metrics.final_wire_bytes = io.bytes_received;
            break;
        }

        fflush(stdout);
        fprintf(stderr, "client: attempt %" PRIu32 " ended (%s)\n", metrics.attempts, report.message);
        if (!is_retryable(status)) {
            break;
        }

        if (retry_delay_ms > 0) {
            sleep_ms(retry_delay_ms);
        }
    }

    metrics.seconds = seconds_since(&start);
    cn_checkpoint_free(&ckpt);

    if (status == CN_CLIENT_OK) {
        printf("Received %" PRIu32 " file(s), %" PRIu64 " verified byte(s) into %s/%s in %.3f s "
               "(%.2f MB/s) after %" PRIu32 " attempt(s)\n", metrics.files, metrics.useful_bytes,
               argv[4], argv[3], metrics.seconds,
               ratio((double)metrics.useful_bytes, metrics.seconds) / 1e6, metrics.attempts);
    }
    print_metrics(stdout, &metrics, status);
    fflush(stdout);
    append_csv(argv[3], &metrics, status);
    return status == CN_CLIENT_OK ? EXIT_SUCCESS : (int)status;
}
