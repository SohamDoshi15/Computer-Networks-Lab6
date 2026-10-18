#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "server_common.h"
#include "fault_injection.h"

#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../common/socket_io.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DATA_CHUNK_SIZE (64u * 1024u)

static int compare_names(const void *a, const void *b)
{
    const struct part1_file *fa = a;
    const struct part1_file *fb = b;
    return strcmp(fa->name, fb->name);
}

int part1_validate_topic_name(const char *topic)
{
    size_t i, length;
    if (topic == NULL || topic[0] == '\0') return 0;
    length = strlen(topic);
    if (length > CN_MAX_TOPIC_SIZE || strcmp(topic, ".") == 0 ||
        strcmp(topic, "..") == 0) return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)topic[i];
        if (c == '/' || c == '\\' || c < 32 || c == 127) return 0;
    }
    return 1;
}

static int hash_file_at(int dir_fd, const char *name, uint64_t *size_out,
                        uint8_t digest_out[CN_SHA256_SIZE])
{
    int fd = -1, result = -1;
    struct stat st;
    EVP_MD_CTX *ctx = NULL;
    unsigned char buffer[65536];
    unsigned int digest_length = 0;
    ssize_t n;

    fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) goto done;

    ctx = EVP_MD_CTX_new();
    if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto done;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (EVP_DigestUpdate(ctx, buffer, (size_t)n) != 1) goto done;
    }
    if (n < 0 || EVP_DigestFinal_ex(ctx, digest_out, &digest_length) != 1 ||
        digest_length != CN_SHA256_SIZE) goto done;

    /* Re-check size after hashing so a changing file is not silently accepted. */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) goto done;
    *size_out = (uint64_t)st.st_size;
    result = 0;

done:
    EVP_MD_CTX_free(ctx);
    if (fd >= 0) close(fd);
    return result;
}

void part1_free_manifest(struct part1_manifest *manifest)
{
    size_t i;
    if (manifest == NULL) return;
    for (i = 0; i < manifest->count; ++i) free(manifest->files[i].name);
    free(manifest->files);
    free(manifest->topic_path);
    memset(manifest, 0, sizeof(*manifest));
}

int part1_build_manifest(const char *root, const char *topic,
                         struct part1_manifest *manifest)
{
    int root_fd = -1, topic_fd = -1, result = -1;
    DIR *directory = NULL;
    struct dirent *entry;
    struct stat st;
    char *topic_path = NULL;
    size_t capacity = 0;

    if (root == NULL || manifest == NULL || !part1_validate_topic_name(topic)) {
        errno = EINVAL;
        return -1;
    }
    memset(manifest, 0, sizeof(*manifest));

    root_fd = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (root_fd < 0) goto done;

    /* Topic is a single validated component; O_NOFOLLOW blocks topic symlinks. */
    topic_fd = openat(root_fd, topic, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (topic_fd < 0) goto done;

    if (fstat(topic_fd, &st) != 0 || !S_ISDIR(st.st_mode)) goto done;

    if (asprintf(&topic_path, "%s/%s", root, topic) < 0) {
        topic_path = NULL;
        goto done;
    }
    directory = fdopendir(dup(topic_fd));
    if (directory == NULL) goto done;

    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct part1_file file;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        /* This service transfers regular files directly inside the topic directory.
           Symlinks, subdirectories, devices, and other special files are skipped. */
        if (fstatat(topic_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) goto done;
        if (!S_ISREG(st.st_mode)) continue;
        if (strlen(entry->d_name) > CN_MAX_FILENAME_SIZE) {
            errno = ENAMETOOLONG;
            goto done;
        }
        memset(&file, 0, sizeof(file));
        file.name = strdup(entry->d_name);
        if (file.name == NULL) goto done;
        if (hash_file_at(topic_fd, file.name, &file.size, file.sha256) != 0) {
            free(file.name);
            goto done;
        }
        if (manifest->count == capacity) {
            size_t new_capacity = capacity == 0 ? 8 : capacity * 2;
            struct part1_file *new_files = realloc(manifest->files,
                                                   new_capacity * sizeof(*new_files));
            if (new_files == NULL) {
                free(file.name);
                goto done;
            }
            manifest->files = new_files;
            capacity = new_capacity;
        }
        if (UINT64_MAX - manifest->total_bytes < file.size) {
            free(file.name);
            errno = EOVERFLOW;
            goto done;
        }
        manifest->files[manifest->count++] = file;
        manifest->total_bytes += file.size;
        errno = 0;
    }
    if (errno != 0) goto done;

    qsort(manifest->files, manifest->count, sizeof(*manifest->files), compare_names);
    manifest->topic_path = topic_path;
    topic_path = NULL;
    result = 0;

done:
    if (directory != NULL) closedir(directory);
    if (topic_fd >= 0) close(topic_fd);
    if (root_fd >= 0) close(root_fd);
    free(topic_path);
    if (result != 0) part1_free_manifest(manifest);
    return result;
}

static int send_payload(int fd, uint8_t type, const uint8_t transfer_id[16],
                        const uint8_t *payload, size_t length)
{
    struct cn_frame_header header;
    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = type;
    header.payload_length = length;
    if (transfer_id != NULL) memcpy(header.transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
    return cn_send_frame(fd, &header, payload);
}

int part1_send_error(int client_fd, const uint8_t transfer_id[16],
                     uint32_t code, const char *message)
{
    uint8_t payload[4 + 4 + CN_MAX_ERROR_TEXT_SIZE];
    struct cn_writer writer;
    size_t length;
    if (message == NULL) message = "request failed";
    length = strlen(message);
    if (length > CN_MAX_ERROR_TEXT_SIZE) length = CN_MAX_ERROR_TEXT_SIZE;
    cn_writer_init(&writer, payload, sizeof(payload));
    if (cn_writer_put_u32(&writer, code) != 0 ||
        cn_writer_put_string(&writer, message, CN_MAX_ERROR_TEXT_SIZE) != 0) return -1;
    return send_payload(client_fd, CN_MSG_ERROR, transfer_id, payload, writer.offset);
}

static int send_manifest(int fd, const uint8_t transfer_id[16],
                         const struct part1_manifest *manifest)
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
    if (manifest->count > UINT32_MAX ||
        cn_writer_put_u32(&writer, (uint32_t)manifest->count) != 0) {
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
    {
        int rc = send_payload(fd, CN_MSG_MANIFEST, transfer_id, payload, writer.offset);
        free(payload);
        return rc == CN_PROTOCOL_OK ? 0 : -1;
    }
}

static int send_file(int fd, int topic_fd, const uint8_t transfer_id[16],
                     uint32_t index, const struct part1_file *file,
                     struct cn_fault_injector *fault, struct cn_serve_stats *stats)
{
    uint8_t start_payload[4 + 4 + CN_MAX_FILENAME_SIZE + 8 + CN_SHA256_SIZE];
    struct cn_writer writer;
    uint8_t buffer[DATA_CHUNK_SIZE];
    uint64_t offset = 0;
    int input_fd = -1, result = -1;
    struct stat st;

    cn_writer_init(&writer, start_payload, sizeof(start_payload));
    if (cn_writer_put_u32(&writer, index) != 0 ||
        cn_writer_put_string(&writer, file->name, CN_MAX_FILENAME_SIZE) != 0 ||
        cn_writer_put_u64(&writer, file->size) != 0 ||
        cn_writer_put_bytes(&writer, file->sha256, CN_SHA256_SIZE) != 0 ||
        send_payload(fd, CN_MSG_FILE_START, transfer_id, start_payload, writer.offset) != CN_PROTOCOL_OK) {
        return -1;
    }

    input_fd = openat(topic_fd, file->name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input_fd < 0 || fstat(input_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size != file->size) goto done;

    while (offset < file->size) {
        size_t want = (file->size - offset) > sizeof(buffer) ? sizeof(buffer) : (size_t)(file->size - offset);
        ssize_t got = read(input_fd, buffer, want);
        uint8_t *data_payload;
        size_t payload_size;
        struct cn_writer data_writer;
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;
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
        {
            int rc;
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
                rc = send_payload(fd, CN_MSG_DATA, transfer_id, data_payload, data_writer.offset);
                if (rc == CN_PROTOCOL_OK && stats != NULL) stats->data_bytes_sent += (uint64_t)got;
            }
            free(data_payload);
            if (rc != CN_PROTOCOL_OK) goto done;
        }
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
            send_payload(fd, CN_MSG_FILE_END, transfer_id, end_payload, end_writer.offset) != CN_PROTOCOL_OK) goto done;
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
            ack_index != index || ack_offset != file->size) {
            free(ack_payload);
            goto done;
        }
        free(ack_payload);
    }
    if (stats != NULL) stats->files_acked += 1;
    result = 0;

done:
    if (input_fd >= 0) close(input_fd);
    return result;
}

int part1_serve_client(int client_fd, const char *root)
{
    return part1_serve_client_ex(client_fd, root, NULL, NULL);
}

static double elapsed_since(const struct timespec *start)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int part1_serve_client_ex(int client_fd, const char *root,
                          struct cn_fault_injector *fault,
                          struct cn_serve_stats *stats)
{
    struct cn_frame_header request;
    uint8_t *request_payload = NULL;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    struct cn_reader reader;
    struct part1_manifest manifest;
    int root_fd = -1, topic_fd = -1;
    size_t i;
    uint8_t complete_payload[12];
    struct cn_writer complete_writer;
    int result = -1;
    struct cn_io_counters *previous_counters = cn_io_get_counters();
    struct timespec start = {0, 0};

    memset(transfer_id, 0, sizeof(transfer_id));
    memset(&manifest, 0, sizeof(manifest));
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
        cn_io_set_counters(&stats->io);
    }
    (void)cn_socket_set_nodelay(client_fd);

    if (cn_recv_frame(client_fd, &request, &request_payload) != CN_PROTOCOL_OK) goto done;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    if (request.message_type != CN_MSG_GET) {
        (void)part1_send_error(client_fd, request.transfer_id, CN_ERR_INVALID_REQUEST, "expected GET");
        goto done;
    }
    cn_reader_init(&reader, request_payload, (size_t)request.payload_length);
    if (cn_reader_get_string(&reader, topic, sizeof(topic), CN_MAX_TOPIC_SIZE) != 0 ||
        cn_reader_remaining(&reader) != 0 || !part1_validate_topic_name(topic)) {
        (void)part1_send_error(client_fd, request.transfer_id, CN_ERR_INVALID_REQUEST, "invalid topic name");
        goto done;
    }
    memcpy(transfer_id, request.transfer_id, CN_TRANSFER_ID_SIZE);
    if (stats != NULL) memcpy(stats->topic, topic, sizeof(stats->topic));

    if (part1_build_manifest(root, topic, &manifest) != 0) {
        uint32_t code = (errno == ENOENT || errno == ENOTDIR) ? CN_ERR_UNKNOWN_TOPIC : CN_ERR_FILE_IO;
        (void)part1_send_error(client_fd, transfer_id, code, "topic unavailable or could not be read");
        goto done;
    }
    if (send_manifest(client_fd, transfer_id, &manifest) != 0) goto done;
    if (stats != NULL) {
        stats->file_count = (uint32_t)manifest.count;
        stats->data_bytes_planned = manifest.total_bytes;
    }
    /* Connection-mode fault decision: made once, now that the volume is known. */
    if (fault != NULL) (void)cn_fault_plan(fault, manifest.total_bytes);

    root_fd = open(root, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (root_fd < 0) goto done;
    topic_fd = openat(root_fd, topic, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (topic_fd < 0) goto done;

    for (i = 0; i < manifest.count; ++i) {
        if (send_file(client_fd, topic_fd, transfer_id, (uint32_t)i, &manifest.files[i],
                      fault, stats) != 0) goto done;
    }

    cn_writer_init(&complete_writer, complete_payload, sizeof(complete_payload));
    if (cn_writer_put_u32(&complete_writer, (uint32_t)manifest.count) != 0 ||
        cn_writer_put_u64(&complete_writer, manifest.total_bytes) != 0 ||
        send_payload(client_fd, CN_MSG_COMPLETE, transfer_id, complete_payload,
                     complete_writer.offset) != CN_PROTOCOL_OK) goto done;
    if (stats != NULL) stats->completed = 1;
    result = 0;

done:
    if (stats != NULL) {
        if (stats->topic[0] != '\0') stats->seconds = elapsed_since(&start);
        cn_io_set_counters(previous_counters);
    }
    free(request_payload);
    if (topic_fd >= 0) close(topic_fd);
    if (root_fd >= 0) close(root_fd);
    part1_free_manifest(&manifest);
    return result;
}
