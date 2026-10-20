#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 2 Enhanced Server: Parallel TCP connections and byte range service.
 *
 * Usage: server <port> <topics_root_dir> <failure_probability>
 *
 * Workers handle both manifest discovery (GET) and disjoint range streaming
 * (ASSIGN_RANGE) with configurable fault injection and session tracking.
 */

#include "fault_injection.h"
#include "server_common.h"
#include "session_store.h"
#include "../common/case2_protocol.h"
#include "../common/cli.h"
#include "../common/enhanced_protocol.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
static struct cn_session_store g_session_store;

static int send_frame(int fd, uint8_t type, const uint8_t transfer_id[16],
                      const uint8_t *payload, size_t length)
{
    struct cn_frame_header header;
    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = type;
    header.payload_length = length;
    if (transfer_id != NULL) {
        memcpy(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
    }
    return cn_send_frame(fd, &header, payload);
}

static int handle_get_request(int client_fd, const struct cn_frame_header *hdr,
                              const uint8_t *payload, const char *root)
{
    char topic[CN_MAX_TOPIC_SIZE + 1];
    struct cn_reader reader;
    struct part1_manifest manifest;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    uint8_t *manifest_payload = NULL;
    size_t manifest_capacity = 4;
    struct cn_writer writer;
    uint8_t manifest_identity[CN_SHA256_SIZE];
    size_t i;
    int rc = -1;

    cn_reader_init(&reader, payload, (size_t)hdr->payload_length);
    if (cn_reader_get_string(&reader, topic, sizeof(topic), CN_MAX_TOPIC_SIZE) != 0) {
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_INVALID_REQUEST, "invalid topic");
    }

    if (!part1_validate_topic_name(topic) ||
        part1_build_manifest(root, topic, &manifest) != 0) {
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_UNKNOWN_TOPIC, "unknown topic");
    }

    memcpy(transfer_id, hdr->transfer_id, CN_TRANSFER_ID_SIZE);
    /* If transfer_id is all zeroes, generate one */
    {
        int all_zero = 1;
        for (i = 0; i < CN_TRANSFER_ID_SIZE; ++i) {
            if (transfer_id[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            for (i = 0; i < CN_TRANSFER_ID_SIZE; ++i) {
                transfer_id[i] = (uint8_t)(rand() & 0xff);
            }
        }
    }

    for (i = 0; i < manifest.count; ++i) {
        manifest_capacity += 4 + 4 + strlen(manifest.files[i].name) + 8 + CN_SHA256_SIZE;
    }
    manifest_payload = malloc(manifest_capacity);
    if (manifest_payload == NULL) {
        part1_free_manifest(&manifest);
        return part1_send_error(client_fd, transfer_id, CN_ERR_INTERNAL, "out of memory");
    }

    cn_writer_init(&writer, manifest_payload, manifest_capacity);
    cn_writer_put_u32(&writer, (uint32_t)manifest.count);
    for (i = 0; i < manifest.count; ++i) {
        const struct part1_file *f = &manifest.files[i];
        cn_writer_put_u32(&writer, (uint32_t)i);
        cn_writer_put_string(&writer, f->name, CN_MAX_FILENAME_SIZE);
        cn_writer_put_u64(&writer, f->size);
        cn_writer_put_bytes(&writer, f->sha256, CN_SHA256_SIZE);
    }

    cn_compute_manifest_identity(manifest_payload, writer.offset, manifest_identity);
    cn_session_store_create(&g_session_store, transfer_id, topic, manifest_identity);

    if (send_frame(client_fd, CN_MSG_MANIFEST, transfer_id, manifest_payload, writer.offset) == CN_PROTOCOL_OK) {
        rc = 0;
    }

    free(manifest_payload);
    part1_free_manifest(&manifest);
    return rc;
}

static int handle_assign_range(int client_fd, const struct cn_frame_header *hdr,
                               const uint8_t *payload, const char *root,
                               const struct worker_args *args,
                               struct cn_fault_injector *fault)
{
    struct cn_range_assignment req;
    struct part1_manifest manifest;
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t ready_buf[CN_RANGE_READY_PAYLOAD_SIZE];
    uint8_t end_buf[CN_RANGE_END_PAYLOAD_SIZE];
    struct cn_range_end end_msg;
    int topic_fd = -1, file_fd = -1, rc = -1;
    uint64_t current_offset;
    uint8_t data_frame_buf[16 + DATA_CHUNK_SIZE];
    struct cn_writer writer;

    if (cn_decode_assign_range(payload, (size_t)hdr->payload_length, &req) != 0) {
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_INVALID_REQUEST, "invalid range request");
    }

    if (cn_session_store_get_topic(&g_session_store, hdr->transfer_id, topic) != 0) {
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_RESUME_REJECTED, "session not found");
    }

    if (part1_build_manifest(root, topic, &manifest) != 0) {
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_UNKNOWN_TOPIC, "topic missing");
    }

    if (req.file_index >= manifest.count) {
        part1_free_manifest(&manifest);
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_RANGE_REJECTED, "file index out of bounds");
    }

    const struct part1_file *target = &manifest.files[req.file_index];
    if (req.start_offset >= req.end_offset || req.end_offset > target->size) {
        part1_free_manifest(&manifest);
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_RANGE_REJECTED, "invalid range offsets");
    }

    /* Send RANGE_READY */
    if (cn_encode_range_ready(&req, ready_buf) != 0 ||
        send_frame(client_fd, CN_MSG_RANGE_READY, hdr->transfer_id, ready_buf, sizeof(ready_buf)) != CN_PROTOCOL_OK) {
        part1_free_manifest(&manifest);
        return -1;
    }

    /* Plan fault injection for the byte range */
    cn_fault_plan_connection(fault, req.end_offset - req.start_offset);

    topic_fd = open(manifest.topic_path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (topic_fd >= 0) {
        file_fd = openat(topic_fd, target->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (file_fd < 0) {
        if (topic_fd >= 0) close(topic_fd);
        part1_free_manifest(&manifest);
        return part1_send_error(client_fd, hdr->transfer_id, CN_ERR_FILE_IO, "failed to open file");
    }

    current_offset = req.start_offset;
    while (current_offset < req.end_offset) {
        uint64_t remaining = req.end_offset - current_offset;
        size_t chunk_len = remaining > DATA_CHUNK_SIZE ? DATA_CHUNK_SIZE : (size_t)remaining;
        ssize_t bytes_read = pread(file_fd, data_frame_buf + 16, chunk_len, (off_t)current_offset);
        if (bytes_read <= 0) {
            break;
        }

        /* Check fault injection */
        if (fault->config.probability > 0.0 && fault->cut_planned) {
            if (fault->data_bytes_sent + (size_t)bytes_read >= fault->cut_at) {
                size_t allowable = (fault->cut_at > fault->data_bytes_sent) ?
                                   (size_t)(fault->cut_at - fault->data_bytes_sent) : 0;
                if (allowable > 0) {
                    cn_writer_init(&writer, data_frame_buf, 16 + allowable);
                    cn_writer_put_u32(&writer, req.file_index);
                    cn_writer_put_u64(&writer, current_offset);
                    cn_writer_put_u32(&writer, (uint32_t)allowable);
                    (void)send_frame(client_fd, CN_MSG_DATA, hdr->transfer_id, data_frame_buf, 16 + allowable);
                }
                fault->fired = 1;
                cn_fault_execute_cut(fault, client_fd);
                rc = -1;
                goto cleanup;
            }
        }

        cn_writer_init(&writer, data_frame_buf, 16 + (size_t)bytes_read);
        cn_writer_put_u32(&writer, req.file_index);
        cn_writer_put_u64(&writer, current_offset);
        cn_writer_put_u32(&writer, (uint32_t)bytes_read);

        if (send_frame(client_fd, CN_MSG_DATA, hdr->transfer_id, data_frame_buf, 16 + (size_t)bytes_read) != CN_PROTOCOL_OK) {
            rc = -1;
            goto cleanup;
        }

        fault->data_bytes_sent += (size_t)bytes_read;
        current_offset += (size_t)bytes_read;
    }

    /* Range completed: send RANGE_END */
    end_msg.range_id = req.range_id;
    end_msg.file_index = req.file_index;
    end_msg.end_offset = current_offset;
    if (cn_encode_range_end(&end_msg, end_buf) == 0 &&
        send_frame(client_fd, CN_MSG_RANGE_END, hdr->transfer_id, end_buf, sizeof(end_buf)) == CN_PROTOCOL_OK) {
        cn_session_store_update_ack(&g_session_store, hdr->transfer_id, req.file_index, current_offset);
        rc = 0;
    }

cleanup:
    if (file_fd >= 0) close(file_fd);
    if (topic_fd >= 0) close(topic_fd);
    part1_free_manifest(&manifest);
    return rc;
}

static void *worker_main(void *arg)
{
    struct worker_args *args = arg;
    struct cn_fault_injector fault;
    struct cn_frame_header hdr;
    uint8_t *payload = NULL;
    enum cn_protocol_result pres;
    int opt = 1;

    setsockopt(args->client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    cn_fault_injector_init(&fault, args->fault_config, args->connection_id);

    pres = cn_recv_frame(args->client_fd, &hdr, &payload);
    if (pres == CN_PROTOCOL_OK) {
        if (hdr.message_type == CN_MSG_GET) {
            handle_get_request(args->client_fd, &hdr, payload, args->root);
        } else if (hdr.message_type == CN_MSG_ASSIGN_RANGE) {
            handle_assign_range(args->client_fd, &hdr, payload, args->root, args, &fault);
        } else {
            part1_send_error(args->client_fd, hdr.transfer_id, CN_ERR_INVALID_REQUEST, "unexpected message");
        }
    }

    free(payload);
    close(args->client_fd);
    free(args);
    return NULL;
}

int main(int argc, char **argv)
{
    uint16_t port;
    const char *root;
    double probability;
    struct cn_fault_config fault_config;
    int listen_fd;
    struct sockaddr_in sin;
    int opt = 1;
    uint64_t next_conn = 1;

    if (cn_cli_parse_part2_server(argc, (const char *const *)argv, &port, &root, &probability) != 0) {
        fprintf(stderr, "usage: server <port> <topics_root_dir> <failure_probability>\n");
        return 1;
    }

    if (cn_fault_config_init(&fault_config, probability) != 0) {
        fprintf(stderr, "server: invalid fault configuration\n");
        return 1;
    }

    if (cn_session_store_init(&g_session_store, 120) != 0) {
        fprintf(stderr, "server: failed to init session store\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
        listen(listen_fd, 128) != 0) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    printf("Enhanced server listening on port %u (fault probability %.4f)\n", (unsigned)port, probability);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        struct worker_args *wargs = malloc(sizeof(*wargs));
        if (wargs == NULL) {
            close(client_fd);
            continue;
        }
        wargs->client_fd = client_fd;
        wargs->connection_id = next_conn++;
        wargs->root = root;
        wargs->fault_config = &fault_config;

        pthread_t thread;
        if (pthread_create(&thread, NULL, worker_main, wargs) != 0) {
            close(client_fd);
            free(wargs);
            continue;
        }
        pthread_detach(thread);
    }

    close(listen_fd);
    cn_session_store_destroy(&g_session_store);
    return 0;
}
