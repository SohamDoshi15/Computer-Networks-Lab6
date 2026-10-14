#define _XOPEN_SOURCE 700

#include "client.h"
#include "protocol.h"
#include "serialization.h"
#include "socket_io.h"

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define PATH_SIZE 1024
#define CHUNK_SIZE (64u * 1024u)

struct fake_file {
    const char *name;
    const uint8_t *data;
    size_t size;
};

struct fake_server;
typedef void (*fake_script)(struct fake_server *server);

/* A scripted server on one end of a loopback TCP connection; the client runs on the other. */
struct fake_server {
    int fd;
    fake_script script;
    const struct fake_file *files;
    uint32_t file_count;
    int fragment;
    int got_get;
    uint8_t client_id[CN_TRANSFER_ID_SIZE];
    uint8_t send_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    int acks;
    int errors;
};

static char base_dir[PATH_SIZE];
static unsigned client_timeout_seconds = 5;

static uint8_t big_data[200000];
static const uint8_t hello_data[] = "hello, integrity\n";
static const uint8_t old_data[] = "previous version";

static char *pathf(char *buffer, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, PATH_SIZE, format, args);
    va_end(args);
    assert(length > 0 && length < PATH_SIZE);
    return buffer;
}

static void sha256(const void *data, size_t size, uint8_t digest[CN_SHA256_SIZE])
{
    unsigned int length = 0;
    assert(EVP_Digest(data, size, digest, &length, EVP_sha256(), NULL) == 1);
    assert(length == CN_SHA256_SIZE);
}

static int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static void write_file(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(data, 1, size, file) == size);
    assert(fclose(file) == 0);
}

static void assert_file_equals(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *actual = malloc(size + 1);
    assert(file != NULL && actual != NULL);
    assert(fread(actual, 1, size + 1, file) == size);
    assert(memcmp(actual, data, size) == 0);
    assert(fclose(file) == 0);
    free(actual);
}

static size_t count_entries(const char *directory_path)
{
    DIR *directory = opendir(directory_path);
    struct dirent *entry;
    size_t count = 0;

    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

static void raw_send(struct fake_server *server, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t index;

    if (!server->fragment) {
        (void)cn_send_all(server->fd, data, length);
        return;
    }
    /* Exercise the client's handling of frames split across many recv() calls. */
    for (index = 0; index < length; ++index) {
        if (cn_send_all(server->fd, bytes + index, 1) != CN_PROTOCOL_OK) {
            return;
        }
    }
}

static void send_frame_header(struct fake_server *server, uint8_t type, uint64_t length)
{
    struct cn_frame_header header;
    uint8_t encoded[CN_FRAME_HEADER_SIZE];

    memset(&header, 0, sizeof(header));
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = type;
    header.payload_length = length;
    memcpy(header.transfer_id, server->send_id, CN_TRANSFER_ID_SIZE);
    assert(cn_encode_header(&header, encoded) == CN_PROTOCOL_OK);
    raw_send(server, encoded, sizeof(encoded));
}

static void send_frame(struct fake_server *server, uint8_t type, const void *payload,
                       size_t length)
{
    send_frame_header(server, type, length);
    raw_send(server, payload, length);
}

static void expect_get(struct fake_server *server)
{
    struct cn_frame_header header;
    struct cn_reader reader;
    uint8_t *payload = NULL;

    assert(cn_recv_frame(server->fd, &header, &payload) == CN_PROTOCOL_OK);
    assert(header.message_type == CN_MSG_GET);
    cn_reader_init(&reader, payload, (size_t)header.payload_length);
    assert(cn_reader_get_string(&reader, server->topic, sizeof(server->topic),
                                CN_MAX_TOPIC_SIZE) == 0);
    assert(cn_reader_remaining(&reader) == 0);
    memcpy(server->client_id, header.transfer_id, CN_TRANSFER_ID_SIZE);
    memcpy(server->send_id, header.transfer_id, CN_TRANSFER_ID_SIZE);
    server->got_get = 1;
    free(payload);
}

/* Reads one frame from the client, counting ACKs and ERRORs. Returns 0 at EOF. */
static int read_client_frame(struct fake_server *server)
{
    struct cn_frame_header header;
    struct cn_reader reader;
    uint8_t *payload = NULL;
    uint32_t index;
    uint64_t offset;
    int type;

    if (cn_recv_frame(server->fd, &header, &payload) != CN_PROTOCOL_OK) {
        return 0;
    }
    assert(memcmp(header.transfer_id, server->client_id, CN_TRANSFER_ID_SIZE) == 0);
    type = header.message_type;
    if (type == CN_MSG_ACK) {
        cn_reader_init(&reader, payload, (size_t)header.payload_length);
        assert(cn_reader_get_u32(&reader, &index) == 0);
        assert(cn_reader_get_u64(&reader, &offset) == 0);
        assert(cn_reader_remaining(&reader) == 0);
        assert(index < server->file_count && offset == server->files[index].size);
        server->acks += 1;
    } else if (type == CN_MSG_ERROR) {
        server->errors += 1;
    }
    free(payload);
    return type;
}

static void put_entry(struct cn_writer *writer, uint32_t index, const struct fake_file *file)
{
    uint8_t digest[CN_SHA256_SIZE];

    sha256(file->data, file->size, digest);
    assert(cn_writer_put_u32(writer, index) == 0);
    assert(cn_writer_put_string(writer, file->name, CN_MAX_FILENAME_SIZE) == 0);
    assert(cn_writer_put_u64(writer, file->size) == 0);
    assert(cn_writer_put_bytes(writer, digest, sizeof(digest)) == 0);
}

static void send_manifest(struct fake_server *server)
{
    uint8_t payload[8192];
    struct cn_writer writer;
    uint32_t index;

    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, server->file_count) == 0);
    for (index = 0; index < server->file_count; ++index) {
        put_entry(&writer, index, &server->files[index]);
    }
    send_frame(server, CN_MSG_MANIFEST, payload, writer.offset);
}

static void send_file_start(struct fake_server *server, uint32_t index)
{
    uint8_t payload[CN_MAX_FILENAME_SIZE + 64];
    struct cn_writer writer;

    cn_writer_init(&writer, payload, sizeof(payload));
    put_entry(&writer, index, &server->files[index]);
    send_frame(server, CN_MSG_FILE_START, payload, writer.offset);
}

static void send_data(struct fake_server *server, uint32_t index, uint64_t offset,
                      const uint8_t *data, size_t length)
{
    uint8_t *payload = malloc(16 + length);
    struct cn_writer writer;

    assert(payload != NULL);
    cn_writer_init(&writer, payload, 16 + length);
    assert(cn_writer_put_u32(&writer, index) == 0);
    assert(cn_writer_put_u64(&writer, offset) == 0);
    assert(cn_writer_put_u32(&writer, (uint32_t)length) == 0);
    assert(cn_writer_put_bytes(&writer, data, length) == 0);
    send_frame(server, CN_MSG_DATA, payload, writer.offset);
    free(payload);
}

static void send_u32_frame(struct fake_server *server, uint8_t type, uint32_t value)
{
    uint8_t payload[4];
    struct cn_writer writer;

    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, value) == 0);
    send_frame(server, type, payload, writer.offset);
}

static void send_file_bytes(struct fake_server *server, uint32_t index, const uint8_t *data,
                            size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        const size_t chunk = length - offset < CHUNK_SIZE ? length - offset : CHUNK_SIZE;
        send_data(server, index, offset, data + offset, chunk);
        offset += chunk;
    }
}

static void send_file(struct fake_server *server, uint32_t index)
{
    send_file_start(server, index);
    send_file_bytes(server, index, server->files[index].data, server->files[index].size);
    send_u32_frame(server, CN_MSG_FILE_END, index);
}

static void send_complete(struct fake_server *server, uint32_t count, uint64_t total)
{
    uint8_t payload[12];
    struct cn_writer writer;

    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, count) == 0);
    assert(cn_writer_put_u64(&writer, total) == 0);
    send_frame(server, CN_MSG_COMPLETE, payload, writer.offset);
}

static uint64_t total_size(const struct fake_server *server)
{
    uint64_t total = 0;
    uint32_t index;

    for (index = 0; index < server->file_count; ++index) {
        total += server->files[index].size;
    }
    return total;
}

static void serve_all(struct fake_server *server)
{
    uint32_t index;

    expect_get(server);
    send_manifest(server);
    for (index = 0; index < server->file_count; ++index) {
        send_file(server, index);
        if (read_client_frame(server) != CN_MSG_ACK) {
            return;
        }
    }
    send_complete(server, server->file_count, total_size(server));
}

static void *server_main(void *opaque)
{
    struct fake_server *server = opaque;

    server->script(server);
    /* Hang up like a real server, but keep reading what the client sends. */
    (void)shutdown(server->fd, SHUT_WR);
    while (read_client_frame(server) != 0) {
    }
    return NULL;
}

/*
 * Connects sockets[1] (client) to sockets[0] (server) over loopback TCP. TCP
 * rather than socketpair(): macOS can lose the EOF wakeup after shutdown() on
 * AF_UNIX sockets, and TCP is what the client really runs on.
 */
static void tcp_pair(int sockets[2])
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    struct timeval timeout;
    const int one = 1;
    int listener = socket(AF_INET, SOCK_STREAM, 0);

    assert(listener >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    sockets[1] = socket(AF_INET, SOCK_STREAM, 0);
    assert(sockets[1] >= 0);
    assert(connect(sockets[1], (struct sockaddr *)&address, sizeof(address)) == 0);
    sockets[0] = accept(listener, NULL, NULL);
    assert(sockets[0] >= 0);
    close(listener);

    /* Send each fragment immediately, and never let a test hang forever. The
       server outlasts the client so the client's own timeout is what fires. */
    assert(setsockopt(sockets[0], IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
    timeout.tv_sec = (time_t)client_timeout_seconds;
    timeout.tv_usec = 0;
    assert(setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    timeout.tv_sec += 10;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
}

static enum cn_client_status run(struct fake_server *server, const char *topic,
                                 const char *output_dir, struct cn_client_report *report)
{
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    enum cn_client_status status;
    pthread_t thread;
    int sockets[2];

    tcp_pair(sockets);
    assert(cn_client_new_transfer_id(transfer_id) == 0);

    server->fd = sockets[0];
    assert(pthread_create(&thread, NULL, server_main, server) == 0);
    status = cn_client_fetch_topic(sockets[1], transfer_id, topic, output_dir, NULL, report);
    close(sockets[1]);
    assert(pthread_join(thread, NULL) == 0);
    close(sockets[0]);

    assert(report->status == status);
    if (server->got_get) {
        assert(strcmp(server->topic, topic) == 0);
        assert(memcmp(server->client_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0);
    }
    return status;
}

static void init_server(struct fake_server *server, fake_script script,
                        const struct fake_file *files, uint32_t file_count)
{
    memset(server, 0, sizeof(*server));
    server->fd = -1;
    server->script = script;
    server->files = files;
    server->file_count = file_count;
}

static const struct fake_file standard_files[] = {
    {"empty.txt", (const uint8_t *)"", 0},
    {"hello.txt", hello_data, sizeof(hello_data) - 1},
    {"big.bin", big_data, sizeof(big_data)}
};

/* ---- unit tests ---------------------------------------------------------- */

static void test_name_validation(void)
{
    char long_name[CN_MAX_TOPIC_SIZE + 2];

    assert(cn_client_name_is_valid("Cane", CN_MAX_TOPIC_SIZE));
    assert(cn_client_name_is_valid(".hidden", CN_MAX_TOPIC_SIZE));
    assert(cn_client_name_is_valid("caf\xc3\xa9", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid(".", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("..", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("../etc", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("a/b", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("/abs", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("a\\b", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("bad\nname", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid("del\x7f", CN_MAX_TOPIC_SIZE));
    assert(!cn_client_name_is_valid(NULL, CN_MAX_TOPIC_SIZE));

    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[CN_MAX_TOPIC_SIZE] = '\0';
    assert(cn_client_name_is_valid(long_name, CN_MAX_TOPIC_SIZE));
    long_name[CN_MAX_TOPIC_SIZE] = 'x';
    long_name[CN_MAX_TOPIC_SIZE + 1] = '\0';
    assert(!cn_client_name_is_valid(long_name, CN_MAX_TOPIC_SIZE));
}

static size_t build_manifest(uint8_t *buffer, size_t capacity, uint32_t count,
                             const uint32_t *indexes, const char *const *names,
                             const uint64_t *sizes)
{
    const uint8_t digest[CN_SHA256_SIZE] = {0};
    struct cn_writer writer;
    uint32_t index;

    cn_writer_init(&writer, buffer, capacity);
    assert(cn_writer_put_u32(&writer, count) == 0);
    for (index = 0; index < count; ++index) {
        assert(cn_writer_put_u32(&writer, indexes[index]) == 0);
        assert(cn_writer_put_string(&writer, names[index], CN_MAX_FILENAME_SIZE) == 0);
        assert(cn_writer_put_u64(&writer, sizes[index]) == 0);
        assert(cn_writer_put_bytes(&writer, digest, sizeof(digest)) == 0);
    }
    return writer.offset;
}

static int parse_names(const char *first, const char *second, const char **reason)
{
    const uint32_t indexes[] = {0, 1};
    const char *names[2];
    const uint64_t sizes[] = {1, 2};
    uint8_t payload[512];
    struct cn_manifest manifest;
    size_t length;
    int result;

    names[0] = first;
    names[1] = second;
    length = build_manifest(payload, sizeof(payload), 2, indexes, names, sizes);
    result = cn_manifest_parse(payload, length, &manifest, reason);
    cn_manifest_free(&manifest);
    return result;
}

static void test_manifest_parse(void)
{
    const uint32_t indexes[] = {0, 1};
    const uint32_t swapped[] = {1, 0};
    const char *names[] = {"a.txt", "b.txt"};
    const uint64_t sizes[] = {10, 20};
    const uint64_t huge[] = {UINT64_C(1) << 63, 1};
    const char *unsafe[] = {"../escape", "a/b", ".", "..", "", "a\\b", "tab\tname"};
    const uint8_t zero_digest[CN_SHA256_SIZE] = {0};
    uint8_t payload[512];
    struct cn_manifest manifest;
    struct cn_writer writer;
    const char *reason = NULL;
    size_t length, index;

    length = build_manifest(payload, sizeof(payload), 2, indexes, names, sizes);
    assert(cn_manifest_parse(payload, length, &manifest, &reason) == 0);
    assert(manifest.count == 2 && manifest.total_bytes == 30);
    assert(strcmp(manifest.entries[1].name, "b.txt") == 0 && manifest.entries[1].size == 20);
    cn_manifest_free(&manifest);

    length = build_manifest(payload, sizeof(payload), 0, NULL, NULL, NULL);
    assert(cn_manifest_parse(payload, length, &manifest, &reason) == 0);
    assert(manifest.count == 0 && manifest.entries == NULL);

    /* Truncated payloads, at every length, are rejected without crashing. */
    length = build_manifest(payload, sizeof(payload), 2, indexes, names, sizes);
    for (index = 0; index < length; ++index) {
        assert(cn_manifest_parse(payload, index, &manifest, &reason) == -1);
    }

    payload[length] = 0;
    assert(cn_manifest_parse(payload, length + 1, &manifest, &reason) == -1);
    assert(strcmp(reason, "trailing bytes after the last entry") == 0);

    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, UINT32_MAX) == 0);
    assert(cn_manifest_parse(payload, writer.offset, &manifest, &reason) == -1);
    assert(strcmp(reason, "entry count does not fit the payload") == 0);

    length = build_manifest(payload, sizeof(payload), 2, swapped, names, sizes);
    assert(cn_manifest_parse(payload, length, &manifest, &reason) == -1);
    assert(strcmp(reason, "file indexes are not sequential") == 0);

    length = build_manifest(payload, sizeof(payload), 2, indexes, names, huge);
    assert(cn_manifest_parse(payload, length, &manifest, &reason) == -1);
    assert(strcmp(reason, "file size exceeds local limits") == 0);

    assert(parse_names("same", "same", &reason) == -1);
    assert(strcmp(reason, "duplicate file name") == 0);
    for (index = 0; index < sizeof(unsafe) / sizeof(unsafe[0]); ++index) {
        assert(parse_names("ok", unsafe[index], &reason) == -1);
        assert(strcmp(reason, "unsafe file name") == 0);
    }

    /* A NUL inside the declared name length must not shorten the name. */
    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, 1) == 0);
    assert(cn_writer_put_u32(&writer, 0) == 0);
    assert(cn_writer_put_u32(&writer, 5) == 0);
    assert(cn_writer_put_bytes(&writer, "a\0bcd", 5) == 0);
    assert(cn_writer_put_u64(&writer, 1) == 0);
    assert(cn_writer_put_bytes(&writer, zero_digest, sizeof(zero_digest)) == 0);
    assert(cn_manifest_parse(payload, writer.offset, &manifest, &reason) == -1);
    assert(strcmp(reason, "unsafe file name") == 0);
}

static void test_sha256_fd(void)
{
    static const uint8_t expected[CN_SHA256_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
        0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t digest[CN_SHA256_SIZE];
    char path[PATH_SIZE];
    int fd;

    write_file(pathf(path, "%s/abc", base_dir), "abc", 3);
    fd = open(path, O_RDONLY);
    assert(fd >= 0);
    assert(cn_sha256_fd(fd, digest) == 0);
    assert(memcmp(digest, expected, sizeof(expected)) == 0);
    close(fd);
}

/* ---- transfers that must succeed ----------------------------------------- */

static void test_full_transfer(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, serve_all, standard_files, 3);
    pathf(out, "%s/full", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_OK);
    assert(report.files_completed == 3);
    assert(report.bytes_completed == sizeof(big_data) + sizeof(hello_data) - 1);
    assert(server.acks == 3 && server.errors == 0);

    assert_file_equals(pathf(path, "%s/Cane/empty.txt", out), "", 0);
    assert_file_equals(pathf(path, "%s/Cane/hello.txt", out), hello_data, sizeof(hello_data) - 1);
    assert_file_equals(pathf(path, "%s/Cane/big.bin", out), big_data, sizeof(big_data));
    /* Nothing but the three files: no temporary files are left behind. */
    assert(count_entries(pathf(path, "%s/Cane", out)) == 3);
}

static void test_fragmented_frames(void)
{
    const struct fake_file files[] = {
        {"hello.txt", hello_data, sizeof(hello_data) - 1},
        {"part.bin", big_data, 3000}
    };
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, serve_all, files, 2);
    server.fragment = 1;
    pathf(out, "%s/fragmented", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_OK);
    assert(server.acks == 2);
    assert_file_equals(pathf(path, "%s/Cane/part.bin", out), big_data, 3000);
}

static void test_empty_topic(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, serve_all, NULL, 0);
    pathf(out, "%s/empty/nested", base_dir);
    assert(run(&server, "Vuoto", out, &report) == CN_CLIENT_OK);
    assert(report.files_completed == 0);
    assert(count_entries(pathf(path, "%s/Vuoto", out)) == 0);
}

static void test_replaces_existing_files(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    pathf(out, "%s/replace", base_dir);
    assert(mkdir(out, 0777) == 0);
    assert(mkdir(pathf(path, "%s/Cane", out), 0777) == 0);
    write_file(pathf(path, "%s/Cane/hello.txt", out), old_data, sizeof(old_data));
    write_file(pathf(path, "%s/Cane/.big.bin.part", out), old_data, sizeof(old_data));

    init_server(&server, serve_all, standard_files, 3);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_OK);
    assert_file_equals(pathf(path, "%s/Cane/hello.txt", out), hello_data, sizeof(hello_data) - 1);
    assert_file_equals(pathf(path, "%s/Cane/big.bin", out), big_data, sizeof(big_data));
    assert(count_entries(pathf(path, "%s/Cane", out)) == 3);
}

/* ---- invalid topics ------------------------------------------------------- */

static void script_unknown_topic(struct fake_server *server)
{
    uint8_t payload[64];
    struct cn_writer writer;

    expect_get(server);
    cn_writer_init(&writer, payload, sizeof(payload));
    assert(cn_writer_put_u32(&writer, CN_ERR_UNKNOWN_TOPIC) == 0);
    assert(cn_writer_put_string(&writer, "no such topic\x1b[2J", CN_MAX_ERROR_TEXT_SIZE) == 0);
    send_frame(server, CN_MSG_ERROR, payload, writer.offset);
}

static void test_unknown_topic(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_unknown_topic, NULL, 0);
    pathf(out, "%s/unknown", base_dir);
    assert(run(&server, "Missing", out, &report) == CN_CLIENT_SERVER_ERROR);
    assert(report.server_error_code == CN_ERR_UNKNOWN_TOPIC);
    assert(strstr(report.message, "no such topic") != NULL);
    assert(strchr(report.message, '\x1b') == NULL);
    /* A rejected topic must not leave an empty directory behind. */
    assert(!path_exists(pathf(path, "%s/Missing", out)));
    assert(server.errors == 0);
}

static void test_invalid_topics_rejected_locally(void)
{
    const char *topics[] = {"", ".", "..", "../etc", "a/b", "a\\b", "bad\nname", NULL};
    char long_topic[CN_MAX_TOPIC_SIZE + 2];
    struct cn_client_report report;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE] = {0};
    char out[PATH_SIZE];
    uint8_t byte;
    size_t index;

    memset(long_topic, 'x', sizeof(long_topic) - 1);
    long_topic[sizeof(long_topic) - 1] = '\0';
    topics[7] = long_topic;
    pathf(out, "%s/invalid", base_dir);

    for (index = 0; index < sizeof(topics) / sizeof(topics[0]); ++index) {
        int sockets[2];
        tcp_pair(sockets);
        assert(cn_client_fetch_topic(sockets[1], transfer_id, topics[index], out, NULL,
                                     &report) == CN_CLIENT_USAGE);
        close(sockets[1]);
        /* Nothing was sent: the server end sees EOF immediately. */
        assert(recv(sockets[0], &byte, 1, 0) == 0);
        close(sockets[0]);
    }
    assert(!path_exists(out));
}

/* ---- disconnects and partial reads ---------------------------------------- */

static void script_disconnect_after_get(struct fake_server *server)
{
    expect_get(server);
}

static void test_disconnect_before_manifest(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_disconnect_after_get, NULL, 0);
    pathf(out, "%s/disconnect_early", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_NETWORK);
    assert(strstr(report.message, "closed the connection") != NULL);
    assert(!path_exists(pathf(path, "%s/Cane", out)));
}

static void script_disconnect_in_data(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    send_file(server, 0);
    if (read_client_frame(server) != CN_MSG_ACK) {
        return;
    }
    send_file_start(server, 1);
    send_file_bytes(server, 1, big_data, sizeof(big_data) / 2);
}

static void test_disconnect_in_data(void)
{
    const struct fake_file files[] = {
        {"hello.txt", hello_data, sizeof(hello_data) - 1},
        {"big.bin", big_data, sizeof(big_data)}
    };
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_disconnect_in_data, files, 2);
    pathf(out, "%s/disconnect_data", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_NETWORK);
    assert(report.files_completed == 1);
    /* The verified file stays; the interrupted one leaves neither a final nor a partial file. */
    assert_file_equals(pathf(path, "%s/Cane/hello.txt", out), hello_data, sizeof(hello_data) - 1);
    assert(!path_exists(pathf(path, "%s/Cane/big.bin", out)));
    assert(!path_exists(pathf(path, "%s/Cane/.big.bin.part", out)));
    assert(count_entries(pathf(path, "%s/Cane", out)) == 1);
}

static void script_truncated_frame(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    send_file_start(server, 0);
    /* Promise a 1000-byte DATA payload, deliver ten bytes, then hang up. */
    send_frame_header(server, CN_MSG_DATA, 1000);
    raw_send(server, big_data, 10);
}

static void test_truncated_frame(void)
{
    const struct fake_file files[] = {{"big.bin", big_data, 984}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_truncated_frame, files, 1);
    pathf(out, "%s/truncated", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_NETWORK);
    assert(strstr(report.message, "middle of a frame") != NULL);
    assert(count_entries(pathf(path, "%s/Cane", out)) == 0);
}

static void script_stall(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    /* Then say nothing, keeping the connection open; the client must time out. */
    while (read_client_frame(server) != 0) {
    }
}

static void test_stalled_server_times_out(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE];

    init_server(&server, script_stall, standard_files, 3);
    pathf(out, "%s/stall", base_dir);
    client_timeout_seconds = 1;
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_NETWORK);
    client_timeout_seconds = 5;
    assert(strstr(report.message, "timed out") != NULL);
}

/* ---- corrupted and inconsistent transfers --------------------------------- */

static void script_corrupt_byte(struct fake_server *server)
{
    uint8_t *corrupt = malloc(server->files[0].size);

    assert(corrupt != NULL);
    memcpy(corrupt, server->files[0].data, server->files[0].size);
    corrupt[server->files[0].size / 2] ^= 0x01;

    expect_get(server);
    send_manifest(server);
    send_file_start(server, 0);
    send_file_bytes(server, 0, corrupt, server->files[0].size);
    send_u32_frame(server, CN_MSG_FILE_END, 0);
    (void)read_client_frame(server);
    free(corrupt);
}

static void test_corrupted_data(void)
{
    const struct fake_file files[] = {{"big.bin", big_data, sizeof(big_data)}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    pathf(out, "%s/corrupt", base_dir);
    assert(mkdir(out, 0777) == 0);
    assert(mkdir(pathf(path, "%s/Cane", out), 0777) == 0);
    write_file(pathf(path, "%s/Cane/big.bin", out), old_data, sizeof(old_data));

    init_server(&server, script_corrupt_byte, files, 1);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_INTEGRITY);
    assert(strstr(report.message, "SHA-256 mismatch") != NULL);
    /* The corrupt file was never ACKed, the server was told why, and the
       previous good copy is untouched. */
    assert(server.acks == 0 && server.errors == 1);
    assert_file_equals(pathf(path, "%s/Cane/big.bin", out), old_data, sizeof(old_data));
    assert(count_entries(pathf(path, "%s/Cane", out)) == 1);
}

static void script_short_file(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    send_file_start(server, 0);
    send_file_bytes(server, 0, server->files[0].data, server->files[0].size - 1);
    send_u32_frame(server, CN_MSG_FILE_END, 0);
}

static void test_short_file(void)
{
    const struct fake_file files[] = {{"hello.txt", hello_data, sizeof(hello_data) - 1}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_short_file, files, 1);
    pathf(out, "%s/short", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_INTEGRITY);
    assert(strstr(report.message, "received 16 bytes, manifest declares 17") != NULL);
    assert(server.acks == 0 && server.errors == 1);
    assert(count_entries(pathf(path, "%s/Cane", out)) == 0);
}

static void script_oversized_file(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    send_file_start(server, 0);
    send_file_bytes(server, 0, big_data, server->files[0].size + 1);
    send_u32_frame(server, CN_MSG_FILE_END, 0);
}

static void test_oversized_file(void)
{
    const struct fake_file files[] = {{"big.bin", big_data, 100}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_oversized_file, files, 1);
    pathf(out, "%s/oversized", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_INTEGRITY);
    assert(strstr(report.message, "more than the declared 100 bytes") != NULL);
    assert(count_entries(pathf(path, "%s/Cane", out)) == 0);
}

static void script_offset_gap(struct fake_server *server)
{
    expect_get(server);
    send_manifest(server);
    send_file_start(server, 0);
    send_data(server, 0, 0, big_data, 10);
    send_data(server, 0, 20, big_data + 20, 10);
}

static void test_out_of_order_data(void)
{
    const struct fake_file files[] = {{"big.bin", big_data, 30}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, script_offset_gap, files, 1);
    pathf(out, "%s/gap", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "offset 20, expected offset 10") != NULL);
    assert(count_entries(pathf(path, "%s/Cane", out)) == 0);
}

static void script_file_start_mismatch(struct fake_server *server)
{
    const struct fake_file *real = server->files;
    const struct fake_file other = {"hello.txt", big_data, 17};

    expect_get(server);
    send_manifest(server);
    server->files = &other;
    send_file_start(server, 0);
    server->files = real;
}

static void test_file_start_mismatch(void)
{
    const struct fake_file files[] = {{"hello.txt", hello_data, sizeof(hello_data) - 1}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE];

    init_server(&server, script_file_start_mismatch, files, 1);
    pathf(out, "%s/start_mismatch", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "FILE_START does not match") != NULL);
}

static void script_wrong_transfer_id(struct fake_server *server)
{
    expect_get(server);
    server->send_id[0] ^= 0xff;
    send_manifest(server);
}

static void test_wrong_transfer_id(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE];

    init_server(&server, script_wrong_transfer_id, standard_files, 3);
    pathf(out, "%s/wrong_id", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "different transfer ID") != NULL);
    assert(!path_exists(out));
}

static void test_unsafe_manifest_name(void)
{
    const struct fake_file files[] = {{"../escape.txt", hello_data, 5}};
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, serve_all, files, 1);
    pathf(out, "%s/unsafe", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "unsafe file name") != NULL);
    assert(!path_exists(pathf(path, "%s/escape.txt", out)));
    assert(!path_exists(pathf(path, "%s/escape.txt", base_dir)));
}

static void script_bad_complete(struct fake_server *server)
{
    uint32_t index;

    expect_get(server);
    send_manifest(server);
    for (index = 0; index < server->file_count; ++index) {
        send_file(server, index);
        if (read_client_frame(server) != CN_MSG_ACK) {
            return;
        }
    }
    send_complete(server, server->file_count + 1, total_size(server));
}

static void test_complete_mismatch(void)
{
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE];

    init_server(&server, script_bad_complete, standard_files, 2);
    pathf(out, "%s/bad_complete", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "COMPLETE reports 3 files") != NULL);
}

static void test_part_name_collision(void)
{
    /* Receiving "a" must not clobber an already received file named ".a.part". */
    const struct fake_file files[] = {
        {".a.part", old_data, sizeof(old_data)},
        {"a", hello_data, sizeof(hello_data) - 1}
    };
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];

    init_server(&server, serve_all, files, 2);
    pathf(out, "%s/part_collision", base_dir);
    assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
    assert(strstr(report.message, "collides") != NULL);
    assert_file_equals(pathf(path, "%s/Cane/.a.part", out), old_data, sizeof(old_data));
    assert(!path_exists(pathf(path, "%s/Cane/a", out)));
}

static void test_case_collision(void)
{
    const struct fake_file files[] = {
        {"README", old_data, sizeof(old_data)},
        {"readme", hello_data, sizeof(hello_data) - 1}
    };
    struct fake_server server;
    struct cn_client_report report;
    char out[PATH_SIZE], path[PATH_SIZE];
    int case_insensitive;

    pathf(out, "%s/case_probe", base_dir);
    write_file(out, "", 0);
    case_insensitive = path_exists(pathf(path, "%s/CASE_PROBE", base_dir));

    init_server(&server, serve_all, files, 2);
    pathf(out, "%s/case_collision", base_dir);
    if (case_insensitive) {
        /* On macOS's default file system the second name would overwrite the first. */
        assert(run(&server, "Cane", out, &report) == CN_CLIENT_PROTOCOL);
        assert_file_equals(pathf(path, "%s/Cane/README", out), old_data, sizeof(old_data));
    } else {
        assert(run(&server, "Cane", out, &report) == CN_CLIENT_OK);
        assert_file_equals(pathf(path, "%s/Cane/readme", out), hello_data, sizeof(hello_data) - 1);
    }
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    size_t index;

    signal(SIGPIPE, SIG_IGN);
    for (index = 0; index < sizeof(big_data); ++index) {
        big_data[index] = (uint8_t)((index * 31u) ^ (index >> 9));
    }
    pathf(base_dir, "%s/cn_test_client.XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
    assert(mkdtemp(base_dir) != NULL);

    test_name_validation();
    test_manifest_parse();
    test_sha256_fd();
    test_full_transfer();
    test_fragmented_frames();
    test_empty_topic();
    test_replaces_existing_files();
    test_unknown_topic();
    test_invalid_topics_rejected_locally();
    test_disconnect_before_manifest();
    test_disconnect_in_data();
    test_truncated_frame();
    test_stalled_server_times_out();
    test_corrupted_data();
    test_short_file();
    test_oversized_file();
    test_out_of_order_data();
    test_file_start_mismatch();
    test_wrong_transfer_id();
    test_unsafe_manifest_name();
    test_complete_mismatch();
    test_part_name_collision();
    test_case_collision();

    assert(nftw(base_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0);
    puts("all client tests passed");
    return 0;
}
