#define _POSIX_C_SOURCE 200809L

#include "client.h"

#include "serialization.h"
#include "socket_io.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* file_index:u32 + name length:u32 + size:u64 + sha256, with an empty name. */
#define MANIFEST_ENTRY_MIN_SIZE (4u + 4u + 8u + CN_SHA256_SIZE)
#define HASH_BUFFER_SIZE (64u * 1024u)
#define HEX_DIGEST_SIZE (2u * CN_SHA256_SIZE + 1u)

struct completed_file {
    dev_t device;
    ino_t inode;
};

struct transfer {
    int socket_fd;
    const uint8_t *transfer_id;
    int dir_fd;
    struct cn_manifest manifest;
    struct completed_file *completed;
    struct cn_file_sink sink;
    struct cn_client_report *report;
    FILE *log;
};

static enum cn_client_status fail(struct cn_client_report *report,
                                  enum cn_client_status status,
                                  const char *format, ...)
{
    va_list args;

    report->status = status;
    va_start(args, format);
    (void)vsnprintf(report->message, sizeof(report->message), format, args);
    va_end(args);
    return status;
}

static const char *message_name(uint8_t message_type)
{
    static const char *const names[] = {
        "", "GET", "MANIFEST", "FILE_START", "DATA", "FILE_END", "ACK", "ERROR",
        "COMPLETE", "CANCEL", "RESUME", "RESUME_OK", "ASSIGN_RANGE",
        "RANGE_READY", "ACK_RANGE", "RANGE_END"
    };
    return cn_message_type_is_valid(message_type) ? names[message_type] : "unknown";
}

static void to_hex(const uint8_t *bytes, size_t length, char *output)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0; index < length; ++index) {
        output[2 * index] = digits[bytes[index] >> 4];
        output[2 * index + 1] = digits[bytes[index] & 0x0fu];
    }
    output[2 * length] = '\0';
}

int cn_client_name_is_valid(const char *name, size_t max_length)
{
    size_t index, length;

    if (name == NULL) {
        return 0;
    }
    length = strlen(name);
    if (length == 0 || length > max_length || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0) {
        return 0;
    }
    for (index = 0; index < length; ++index) {
        const unsigned char c = (unsigned char)name[index];
        if (c == '/' || c == '\\' || c < 32 || c == 127) {
            return 0;
        }
    }
    return 1;
}

/* Reads the index, name, size, and digest shared by MANIFEST and FILE_START. */
static int read_entry(struct cn_reader *reader, struct cn_manifest_entry *entry,
                      const char **reason)
{
    char name[CN_MAX_FILENAME_SIZE + 1];
    size_t name_start;

    memset(entry, 0, sizeof(*entry));
    if (cn_reader_get_u32(reader, &entry->file_index) != 0) {
        *reason = "malformed entry";
        return -1;
    }
    name_start = reader->offset;
    if (cn_reader_get_string(reader, name, sizeof(name), CN_MAX_FILENAME_SIZE) != 0) {
        *reason = "malformed entry";
        return -1;
    }
    /* An embedded NUL would silently shorten the name written to disk. */
    if (strlen(name) != reader->offset - name_start - 4 ||
        !cn_client_name_is_valid(name, CN_MAX_FILENAME_SIZE)) {
        *reason = "unsafe file name";
        return -1;
    }
    if (cn_reader_get_u64(reader, &entry->size) != 0 ||
        cn_reader_get_bytes(reader, entry->sha256, CN_SHA256_SIZE) != 0) {
        *reason = "malformed entry";
        return -1;
    }
    if (entry->size > (uint64_t)INT64_MAX) {
        *reason = "file size exceeds local limits";
        return -1;
    }
    entry->name = strdup(name);
    if (entry->name == NULL) {
        *reason = "out of memory";
        return -1;
    }
    return 0;
}

static int compare_name_pointers(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

int cn_manifest_parse(const uint8_t *payload, size_t length,
                      struct cn_manifest *manifest, const char **reason)
{
    struct cn_reader reader;
    const char **names = NULL;
    const char *ignored;
    uint32_t count, index;

    if (reason == NULL) {
        reason = &ignored;
    }
    *reason = NULL;
    memset(manifest, 0, sizeof(*manifest));
    cn_reader_init(&reader, payload, length);

    /* Bound the count by the payload before allocating anything for it. */
    if (cn_reader_get_u32(&reader, &count) != 0 ||
        count > cn_reader_remaining(&reader) / MANIFEST_ENTRY_MIN_SIZE) {
        *reason = "entry count does not fit the payload";
        return -1;
    }
    if (count != 0) {
        manifest->entries = calloc(count, sizeof(*manifest->entries));
        names = malloc(count * sizeof(*names));
        if (manifest->entries == NULL || names == NULL) {
            *reason = "out of memory";
            goto fail;
        }
    }
    for (index = 0; index < count; ++index) {
        struct cn_manifest_entry *entry = &manifest->entries[index];
        if (read_entry(&reader, entry, reason) != 0) {
            goto fail;
        }
        manifest->count = index + 1;
        if (entry->file_index != index) {
            *reason = "file indexes are not sequential";
            goto fail;
        }
        if (UINT64_MAX - manifest->total_bytes < entry->size) {
            *reason = "total size overflows";
            goto fail;
        }
        manifest->total_bytes += entry->size;
        names[index] = entry->name;
    }
    if (cn_reader_remaining(&reader) != 0) {
        *reason = "trailing bytes after the last entry";
        goto fail;
    }
    if (count > 1) {
        qsort(names, count, sizeof(*names), compare_name_pointers);
        for (index = 1; index < count; ++index) {
            if (strcmp(names[index - 1], names[index]) == 0) {
                *reason = "duplicate file name";
                goto fail;
            }
        }
    }
    free(names);
    return 0;

fail:
    free(names);
    cn_manifest_free(manifest);
    return -1;
}

void cn_manifest_free(struct cn_manifest *manifest)
{
    uint32_t index;

    if (manifest == NULL) {
        return;
    }
    for (index = 0; index < manifest->count; ++index) {
        free(manifest->entries[index].name);
    }
    free(manifest->entries);
    memset(manifest, 0, sizeof(*manifest));
}

int cn_sha256_fd(int fd, uint8_t digest[CN_SHA256_SIZE])
{
    uint8_t buffer[HASH_BUFFER_SIZE];
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned int digest_length = 0;
    off_t offset = 0;
    int result = -1;

    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        goto done;
    }
    for (;;) {
        const ssize_t got = pread(fd, buffer, sizeof(buffer), offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            goto done;
        }
        if (got == 0) {
            break;
        }
        if (EVP_DigestUpdate(context, buffer, (size_t)got) != 1) {
            goto done;
        }
        offset += got;
    }
    if (EVP_DigestFinal_ex(context, digest, &digest_length) == 1 &&
        digest_length == CN_SHA256_SIZE) {
        result = 0;
    }

done:
    EVP_MD_CTX_free(context);
    return result;
}

char *cn_part_name(const char *name)
{
    const size_t length = strlen(name);
    char *part = malloc(length + sizeof(".") + sizeof(".part") - 1);

    if (part != NULL) {
        part[0] = '.';
        memcpy(part + 1, name, length);
        memcpy(part + 1 + length, ".part", sizeof(".part"));
    }
    return part;
}

static int make_directories(const char *path)
{
    char *copy;
    char *cursor;
    int saved_errno;

    if (path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    copy = strdup(path);
    if (copy == NULL) {
        return -1;
    }
    for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
            goto fail;
        }
        *cursor = '/';
    }
    if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
        goto fail;
    }
    free(copy);
    return 0;

fail:
    saved_errno = errno;
    free(copy);
    errno = saved_errno;
    return -1;
}

int cn_client_open_topic_dir(const char *output_dir, const char *topic)
{
    int base_fd, topic_fd = -1, saved_errno;

    if (output_dir == NULL || !cn_client_name_is_valid(topic, CN_MAX_TOPIC_SIZE)) {
        errno = EINVAL;
        return -1;
    }
    if (make_directories(output_dir) != 0) {
        return -1;
    }
    base_fd = open(output_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base_fd < 0) {
        return -1;
    }
    if (mkdirat(base_fd, topic, 0777) == 0 || errno == EEXIST) {
        /* O_NOFOLLOW keeps a planted symlink from redirecting the topic. */
        topic_fd = openat(base_fd, topic, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    saved_errno = errno;
    close(base_fd);
    errno = saved_errno;
    return topic_fd;
}

int cn_file_sink_open(struct cn_file_sink *sink, int dir_fd,
                      const struct cn_manifest_entry *entry)
{
    int saved_errno;

    *sink = (struct cn_file_sink)CN_FILE_SINK_INIT;
    sink->dir_fd = dir_fd;
    sink->entry = entry;
    sink->temp_name = cn_part_name(entry->name);
    if (sink->temp_name == NULL) {
        return -1;
    }
    /* Case 1 discards partial progress, so a stale partial file is replaced. */
    if (unlinkat(dir_fd, sink->temp_name, 0) != 0 && errno != ENOENT) {
        goto fail;
    }
    sink->fd = openat(dir_fd, sink->temp_name,
                      O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (sink->fd < 0) {
        goto fail;
    }
    return 0;

fail:
    saved_errno = errno;
    free(sink->temp_name);
    sink->temp_name = NULL;
    errno = saved_errno;
    return -1;
}

int cn_file_sink_open_resume(struct cn_file_sink *sink, int dir_fd,
                             const struct cn_manifest_entry *entry,
                             uint64_t resume_offset)
{
    int saved_errno;

    *sink = (struct cn_file_sink)CN_FILE_SINK_INIT;
    sink->dir_fd = dir_fd;
    sink->entry = entry;
    sink->temp_name = cn_part_name(entry->name);
    if (sink->temp_name == NULL) {
        return -1;
    }
    sink->fd = openat(dir_fd, sink->temp_name,
                      O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (sink->fd < 0) {
        goto fail;
    }
    if (ftruncate(sink->fd, (off_t)resume_offset) != 0) {
        goto fail;
    }
    sink->received = resume_offset;
    return 0;

fail:
    saved_errno = errno;
    if (sink->fd >= 0) {
        close(sink->fd);
        sink->fd = -1;
    }
    free(sink->temp_name);
    sink->temp_name = NULL;
    errno = saved_errno;
    return -1;
}

int cn_file_sink_write(struct cn_file_sink *sink, uint64_t offset,
                       const uint8_t *data, size_t length)
{
    size_t written = 0;

    if (sink->fd < 0 || offset > sink->entry->size ||
        length > sink->entry->size - offset) {
        errno = EINVAL;
        return -1;
    }
    while (written < length) {
        const ssize_t result = pwrite(sink->fd, data + written, length - written,
                                      (off_t)(offset + written));
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            if (result == 0) {
                errno = EIO;
            }
            return -1;
        }
        written += (size_t)result;
    }
    sink->received += length;
    return 0;
}

enum cn_client_status cn_file_sink_commit(struct cn_file_sink *sink)
{
    struct stat st;
    int close_result;

    if (sink->fd < 0 || sink->temp_name == NULL) {
        errno = EINVAL;
        return CN_CLIENT_LOCAL_IO;
    }
    if (fsync(sink->fd) != 0 || fstat(sink->fd, &st) != 0) {
        return CN_CLIENT_LOCAL_IO;
    }
    sink->actual_size = (uint64_t)st.st_size;
    if (sink->actual_size != sink->entry->size) {
        return CN_CLIENT_INTEGRITY;
    }
    /* Hash what reached the disk, not what arrived, so the write path is covered too. */
    if (cn_sha256_fd(sink->fd, sink->actual_sha256) != 0) {
        return CN_CLIENT_LOCAL_IO;
    }
    if (memcmp(sink->actual_sha256, sink->entry->sha256, CN_SHA256_SIZE) != 0) {
        return CN_CLIENT_INTEGRITY;
    }
    close_result = close(sink->fd);
    sink->fd = -1;
    if (close_result != 0 ||
        renameat(sink->dir_fd, sink->temp_name, sink->dir_fd, sink->entry->name) != 0) {
        return CN_CLIENT_LOCAL_IO;
    }
    free(sink->temp_name);
    sink->temp_name = NULL;
    /* Make the rename durable before the server is told the file is persisted. */
    if (fsync(sink->dir_fd) != 0 && errno != EINVAL) {
        return CN_CLIENT_LOCAL_IO;
    }
    return CN_CLIENT_OK;
}

void cn_file_sink_abort(struct cn_file_sink *sink)
{
    if (sink->fd >= 0) {
        close(sink->fd);
        sink->fd = -1;
    }
    if (sink->temp_name != NULL) {
        (void)unlinkat(sink->dir_fd, sink->temp_name, 0);
        free(sink->temp_name);
        sink->temp_name = NULL;
    }
}

int cn_client_new_transfer_id(uint8_t transfer_id[CN_TRANSFER_ID_SIZE])
{
    size_t filled = 0;
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return -1;
    }
    while (filled < CN_TRANSFER_ID_SIZE) {
        const ssize_t got = read(fd, transfer_id + filled, CN_TRANSFER_ID_SIZE - filled);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            close(fd);
            return -1;
        }
        filled += (size_t)got;
    }
    close(fd);
    return 0;
}

int cn_client_connect(const char *host, uint16_t port, unsigned timeout_seconds,
                      struct cn_client_report *report)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct timeval timeout;
    char service[8];
    int socket_fd = -1, saved_errno = ECONNREFUSED, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    rc = getaddrinfo(host, service, &hints, &addresses);
    if (rc != 0) {
        fail(report, CN_CLIENT_NETWORK, "cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    timeout.tv_sec = (time_t)timeout_seconds;
    timeout.tv_usec = 0;
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) {
            saved_errno = errno;
            continue;
        }
        /* Bound every blocking send and recv so a stalled server cannot hang us. */
        if ((timeout_seconds == 0 ||
             (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
              setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0)) &&
            connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            (void)cn_socket_set_nodelay(socket_fd);
            break;
        }
        saved_errno = errno;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);

    if (socket_fd < 0) {
        fail(report, CN_CLIENT_NETWORK, "cannot connect to %s:%u: %s", host,
             (unsigned)port, strerror(saved_errno));
    }
    return socket_fd;
}

static int send_message(const struct transfer *t, uint8_t message_type,
                        const uint8_t *payload, size_t length)
{
    struct cn_frame_header header;

    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = message_type;
    header.payload_length = length;
    memcpy(header.transfer_id, t->transfer_id, CN_TRANSFER_ID_SIZE);
    return cn_send_frame(t->socket_fd, &header, payload);
}

static enum cn_client_status server_error(struct transfer *t, const uint8_t *payload,
                                          size_t length)
{
    char text[CN_MAX_ERROR_TEXT_SIZE + 1];
    struct cn_reader reader;
    uint32_t code;
    size_t index;

    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &code) != 0 ||
        cn_reader_get_string(&reader, text, sizeof(text), CN_MAX_ERROR_TEXT_SIZE) != 0) {
        return fail(t->report, CN_CLIENT_PROTOCOL, "malformed ERROR frame from server");
    }
    /* The text is untrusted; keep control characters away from the terminal. */
    for (index = 0; text[index] != '\0'; ++index) {
        if ((unsigned char)text[index] < 32 || text[index] == 127) {
            text[index] = '?';
        }
    }
    t->report->server_error_code = code;
    return fail(t->report, CN_CLIENT_SERVER_ERROR, "server error %" PRIu32 ": %s", code, text);
}

/* Receives one frame of this transfer. An ERROR frame becomes CN_CLIENT_SERVER_ERROR. */
static enum cn_client_status recv_message(struct transfer *t, struct cn_frame_header *header,
                                          uint8_t **payload)
{
    enum cn_client_status status;

    switch (cn_recv_frame(t->socket_fd, header, payload)) {
    case CN_PROTOCOL_OK:
        break;
    case CN_PROTOCOL_EOF:
        return fail(t->report, CN_CLIENT_NETWORK, "server closed the connection");
    case CN_PROTOCOL_TRUNCATED:
        return fail(t->report, CN_CLIENT_NETWORK, "server disconnected in the middle of a frame");
    case CN_PROTOCOL_IO_ERROR:
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return fail(t->report, CN_CLIENT_NETWORK, "timed out waiting for the server");
        }
        return fail(t->report, CN_CLIENT_NETWORK, "receive failed: %s", strerror(errno));
    case CN_PROTOCOL_NO_MEMORY:
        return fail(t->report, CN_CLIENT_LOCAL_IO, "out of memory while receiving a frame");
    default:
        return fail(t->report, CN_CLIENT_PROTOCOL, "malformed frame header from server");
    }

    if (memcmp(header->transfer_id, t->transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
        status = fail(t->report, CN_CLIENT_PROTOCOL, "%s frame carries a different transfer ID",
                      message_name(header->message_type));
    } else if (header->message_type == CN_MSG_ERROR) {
        status = server_error(t, *payload, (size_t)header->payload_length);
    } else {
        return CN_CLIENT_OK;
    }
    free(*payload);
    *payload = NULL;
    return status;
}

/* True when name already refers to a file stored earlier in this transfer, for
   example "A" and "a" on a case-insensitive file system. */
static int names_completed_file(const struct transfer *t, const char *name)
{
    struct stat st;
    uint32_t index;

    if (fstatat(t->dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return 0;
    }
    for (index = 0; index < t->report->files_completed; ++index) {
        if (t->completed[index].device == st.st_dev && t->completed[index].inode == st.st_ino) {
            return 1;
        }
    }
    return 0;
}

static enum cn_client_status expect_file_start(struct transfer *t,
                                               const struct cn_manifest_entry *entry)
{
    struct cn_frame_header header;
    struct cn_manifest_entry start;
    struct cn_reader reader;
    uint8_t *payload = NULL;
    const char *reason = NULL;
    enum cn_client_status status;
    int matches;

    status = recv_message(t, &header, &payload);
    if (status != CN_CLIENT_OK) {
        return status;
    }
    if (header.message_type != CN_MSG_FILE_START) {
        free(payload);
        return fail(t->report, CN_CLIENT_PROTOCOL, "expected FILE_START for %s, received %s",
                    entry->name, message_name(header.message_type));
    }
    cn_reader_init(&reader, payload, (size_t)header.payload_length);
    matches = read_entry(&reader, &start, &reason) == 0 &&
              cn_reader_remaining(&reader) == 0 &&
              start.file_index == entry->file_index &&
              strcmp(start.name, entry->name) == 0 &&
              start.size == entry->size &&
              memcmp(start.sha256, entry->sha256, CN_SHA256_SIZE) == 0;
    free(start.name);
    free(payload);
    if (!matches) {
        return fail(t->report, CN_CLIENT_PROTOCOL,
                    "FILE_START does not match manifest entry %" PRIu32 " (%s)",
                    entry->file_index, entry->name);
    }
    return CN_CLIENT_OK;
}

static enum cn_client_status write_data(struct transfer *t, const struct cn_manifest_entry *entry,
                                        const uint8_t *payload, size_t length)
{
    struct cn_reader reader;
    uint32_t file_index, data_length;
    uint64_t offset;

    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &file_index) != 0 ||
        cn_reader_get_u64(&reader, &offset) != 0 ||
        cn_reader_get_u32(&reader, &data_length) != 0 ||
        data_length != cn_reader_remaining(&reader)) {
        return fail(t->report, CN_CLIENT_PROTOCOL, "malformed DATA frame for %s", entry->name);
    }
    if (file_index != entry->file_index) {
        return fail(t->report, CN_CLIENT_PROTOCOL,
                    "DATA for file %" PRIu32 " while receiving file %" PRIu32,
                    file_index, entry->file_index);
    }
    if (offset != t->sink.received) {
        return fail(t->report, CN_CLIENT_PROTOCOL,
                    "DATA for %s at offset %" PRIu64 ", expected offset %" PRIu64,
                    entry->name, offset, t->sink.received);
    }
    if (data_length > entry->size - offset) {
        return fail(t->report, CN_CLIENT_INTEGRITY,
                    "%s: server sent more than the declared %" PRIu64 " bytes",
                    entry->name, entry->size);
    }
    t->report->data_bytes_received += data_length;
    if (cn_file_sink_write(&t->sink, offset, payload + reader.offset, data_length) != 0) {
        return fail(t->report, CN_CLIENT_LOCAL_IO, "cannot write %s: %s", entry->name,
                    strerror(errno));
    }
    return CN_CLIENT_OK;
}

static enum cn_client_status receive_file(struct transfer *t,
                                          const struct cn_manifest_entry *entry)
{
    struct cn_frame_header header;
    struct cn_reader reader;
    struct stat st;
    uint8_t *payload = NULL;
    uint8_t ack[4 + 8];
    struct cn_writer writer;
    char expected_hex[HEX_DIGEST_SIZE], actual_hex[HEX_DIGEST_SIZE];
    enum cn_client_status status;
    char *part_name;
    int aliased;

    part_name = cn_part_name(entry->name);
    if (part_name == NULL) {
        return fail(t->report, CN_CLIENT_LOCAL_IO, "out of memory");
    }
    aliased = names_completed_file(t, part_name) || names_completed_file(t, entry->name);
    free(part_name);
    if (aliased) {
        return fail(t->report, CN_CLIENT_PROTOCOL,
                    "%s collides with a file already received in this transfer", entry->name);
    }

    status = expect_file_start(t, entry);
    if (status != CN_CLIENT_OK) {
        return status;
    }
    if (cn_file_sink_open(&t->sink, t->dir_fd, entry) != 0) {
        return fail(t->report, CN_CLIENT_LOCAL_IO, "cannot create a temporary file for %s: %s",
                    entry->name, strerror(errno));
    }

    for (;;) {
        status = recv_message(t, &header, &payload);
        if (status != CN_CLIENT_OK) {
            return status;
        }
        if (header.message_type == CN_MSG_FILE_END) {
            uint32_t end_index;
            int valid;
            cn_reader_init(&reader, payload, (size_t)header.payload_length);
            valid = cn_reader_get_u32(&reader, &end_index) == 0 &&
                    cn_reader_remaining(&reader) == 0 && end_index == entry->file_index;
            free(payload);
            if (!valid) {
                return fail(t->report, CN_CLIENT_PROTOCOL, "malformed FILE_END for %s",
                            entry->name);
            }
            break;
        }
        if (header.message_type != CN_MSG_DATA) {
            free(payload);
            return fail(t->report, CN_CLIENT_PROTOCOL, "expected DATA or FILE_END for %s, received %s",
                        entry->name, message_name(header.message_type));
        }
        status = write_data(t, entry, payload, (size_t)header.payload_length);
        free(payload);
        payload = NULL;
        if (status != CN_CLIENT_OK) {
            return status;
        }
    }

    status = cn_file_sink_commit(&t->sink);
    if (status == CN_CLIENT_INTEGRITY && t->sink.actual_size != entry->size) {
        return fail(t->report, status, "%s: received %" PRIu64 " bytes, manifest declares %" PRIu64,
                    entry->name, t->sink.actual_size, entry->size);
    }
    to_hex(entry->sha256, CN_SHA256_SIZE, expected_hex);
    if (status == CN_CLIENT_INTEGRITY) {
        to_hex(t->sink.actual_sha256, CN_SHA256_SIZE, actual_hex);
        return fail(t->report, status, "%s: SHA-256 mismatch (expected %s, received %s)",
                    entry->name, expected_hex, actual_hex);
    }
    if (status != CN_CLIENT_OK ||
        fstatat(t->dir_fd, entry->name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return fail(t->report, CN_CLIENT_LOCAL_IO, "cannot store %s: %s", entry->name,
                    strerror(errno));
    }
    t->completed[t->report->files_completed].device = st.st_dev;
    t->completed[t->report->files_completed].inode = st.st_ino;
    t->report->files_completed += 1;
    t->report->bytes_completed += entry->size;

    /* ACK only after the verified file is renamed into place and flushed. */
    cn_writer_init(&writer, ack, sizeof(ack));
    if (cn_writer_put_u32(&writer, entry->file_index) != 0 ||
        cn_writer_put_u64(&writer, entry->size) != 0 ||
        send_message(t, CN_MSG_ACK, ack, writer.offset) != CN_PROTOCOL_OK) {
        return fail(t->report, CN_CLIENT_NETWORK, "could not acknowledge %s: %s", entry->name,
                    strerror(errno));
    }
    if (t->log != NULL) {
        fprintf(t->log, "  verified %s (%" PRIu64 " bytes, sha256 %s)\n", entry->name,
                entry->size, expected_hex);
    }
    return CN_CLIENT_OK;
}

static enum cn_client_status expect_complete(struct transfer *t)
{
    struct cn_frame_header header;
    struct cn_reader reader;
    uint8_t *payload = NULL;
    uint32_t file_count;
    uint64_t total_bytes;
    enum cn_client_status status;
    int valid;

    status = recv_message(t, &header, &payload);
    if (status != CN_CLIENT_OK) {
        return status;
    }
    if (header.message_type != CN_MSG_COMPLETE) {
        free(payload);
        return fail(t->report, CN_CLIENT_PROTOCOL, "expected COMPLETE, received %s",
                    message_name(header.message_type));
    }
    cn_reader_init(&reader, payload, (size_t)header.payload_length);
    valid = cn_reader_get_u32(&reader, &file_count) == 0 &&
            cn_reader_get_u64(&reader, &total_bytes) == 0 &&
            cn_reader_remaining(&reader) == 0;
    free(payload);
    if (!valid) {
        return fail(t->report, CN_CLIENT_PROTOCOL, "malformed COMPLETE frame");
    }
    if (file_count != t->manifest.count || total_bytes != t->manifest.total_bytes) {
        return fail(t->report, CN_CLIENT_PROTOCOL,
                    "COMPLETE reports %" PRIu32 " files and %" PRIu64
                    " bytes, manifest declared %" PRIu32 " and %" PRIu64,
                    file_count, total_bytes, t->manifest.count, t->manifest.total_bytes);
    }
    return CN_CLIENT_OK;
}

/* Best effort: tell the server why the client is abandoning the transfer. */
static void send_failure(const struct transfer *t)
{
    uint8_t payload[4 + 4 + sizeof(t->report->message)];
    struct cn_writer writer;
    const uint32_t code = t->report->status == CN_CLIENT_LOCAL_IO ? CN_ERR_FILE_IO
                                                                  : CN_ERR_PROTOCOL;

    cn_writer_init(&writer, payload, sizeof(payload));
    if (cn_writer_put_u32(&writer, code) == 0 &&
        cn_writer_put_string(&writer, t->report->message, CN_MAX_ERROR_TEXT_SIZE) == 0) {
        (void)send_message(t, CN_MSG_ERROR, payload, writer.offset);
    }
}

enum cn_client_status cn_client_fetch_topic(int socket_fd,
                                            const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                            const char *topic, const char *output_dir,
                                            FILE *log, struct cn_client_report *report)
{
    struct transfer t;
    struct cn_frame_header header;
    struct cn_writer writer;
    uint8_t request[4 + CN_MAX_TOPIC_SIZE];
    uint8_t *payload = NULL;
    const char *reason = NULL;
    enum cn_client_status status;
    uint32_t index;

    memset(report, 0, sizeof(*report));
    memset(&t, 0, sizeof(t));
    t.socket_fd = socket_fd;
    t.transfer_id = transfer_id;
    t.dir_fd = -1;
    t.sink = (struct cn_file_sink)CN_FILE_SINK_INIT;
    t.report = report;
    t.log = log;

    if (!cn_client_name_is_valid(topic, CN_MAX_TOPIC_SIZE)) {
        return fail(report, CN_CLIENT_USAGE,
                    "invalid topic name: use 1-%u bytes without '/', '\\' or control "
                    "characters, and not '.' or '..'", CN_MAX_TOPIC_SIZE);
    }
    if (output_dir == NULL || output_dir[0] == '\0') {
        return fail(report, CN_CLIENT_USAGE, "output directory must not be empty");
    }

    cn_writer_init(&writer, request, sizeof(request));
    if (cn_writer_put_string(&writer, topic, CN_MAX_TOPIC_SIZE) != 0 ||
        send_message(&t, CN_MSG_GET, request, writer.offset) != CN_PROTOCOL_OK) {
        return fail(report, CN_CLIENT_NETWORK, "could not send GET: %s", strerror(errno));
    }

    status = recv_message(&t, &header, &payload);
    if (status != CN_CLIENT_OK) {
        goto done;
    }
    if (header.message_type != CN_MSG_MANIFEST) {
        status = fail(report, CN_CLIENT_PROTOCOL, "expected MANIFEST, received %s",
                      message_name(header.message_type));
        goto done;
    }
    if (cn_manifest_parse(payload, (size_t)header.payload_length, &t.manifest, &reason) != 0) {
        status = fail(report, CN_CLIENT_PROTOCOL, "invalid MANIFEST: %s", reason);
        goto done;
    }
    free(payload);
    payload = NULL;

    /* The topic directory appears only once the server has accepted the topic. */
    t.dir_fd = cn_client_open_topic_dir(output_dir, topic);
    if (t.dir_fd < 0) {
        status = fail(report, CN_CLIENT_LOCAL_IO, "cannot create %s/%s: %s", output_dir, topic,
                      strerror(errno));
        goto done;
    }
    if (t.manifest.count != 0) {
        t.completed = calloc(t.manifest.count, sizeof(*t.completed));
        if (t.completed == NULL) {
            status = fail(report, CN_CLIENT_LOCAL_IO, "out of memory");
            goto done;
        }
    }
    if (log != NULL) {
        fprintf(log, "Topic %s: %" PRIu32 " file(s), %" PRIu64 " byte(s)\n", topic,
                t.manifest.count, t.manifest.total_bytes);
    }

    for (index = 0; index < t.manifest.count; ++index) {
        status = receive_file(&t, &t.manifest.entries[index]);
        if (status != CN_CLIENT_OK) {
            goto done;
        }
    }
    status = expect_complete(&t);

done:
    if (status == CN_CLIENT_PROTOCOL || status == CN_CLIENT_INTEGRITY ||
        status == CN_CLIENT_LOCAL_IO) {
        send_failure(&t);
    }
    cn_file_sink_abort(&t.sink);
    free(payload);
    free(t.completed);
    cn_manifest_free(&t.manifest);
    if (t.dir_fd >= 0) {
        close(t.dir_fd);
    }
    return status;
}
