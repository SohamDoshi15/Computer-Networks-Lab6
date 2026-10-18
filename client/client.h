#ifndef CN_CLIENT_H
#define CN_CLIENT_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Outcome of a client operation. The values double as process exit codes. */
enum cn_client_status {
    CN_CLIENT_OK = 0,
    CN_CLIENT_USAGE = 1,        /* invalid arguments or topic, rejected locally */
    CN_CLIENT_SERVER_ERROR = 2, /* server answered with an ERROR frame */
    CN_CLIENT_NETWORK = 3,      /* connect failure, disconnect, timeout, short frame */
    CN_CLIENT_PROTOCOL = 4,     /* malformed, unexpected, or inconsistent frame */
    CN_CLIENT_INTEGRITY = 5,    /* received file failed size or SHA-256 checks */
    CN_CLIENT_LOCAL_IO = 6      /* output directory or file could not be written */
};

struct cn_client_report {
    enum cn_client_status status;
    uint32_t server_error_code;
    uint32_t files_completed;
    uint64_t bytes_completed;
    uint64_t data_bytes_received; /* file bytes in every complete DATA frame, kept or not */
    char message[512];
};

struct cn_manifest_entry {
    uint32_t file_index;
    char *name;
    uint64_t size;
    uint8_t sha256[CN_SHA256_SIZE];
};

struct cn_manifest {
    struct cn_manifest_entry *entries;
    uint32_t count;
    uint64_t total_bytes;
};

/*
 * One file being received. Bytes go to "<dir>/.<name>.part"; the final name
 * appears only after cn_file_sink_commit() has verified size and SHA-256.
 */
struct cn_file_sink {
    int dir_fd;
    int fd;
    const struct cn_manifest_entry *entry;
    char *temp_name;
    uint64_t received;
    uint64_t actual_size;
    uint8_t actual_sha256[CN_SHA256_SIZE];
};

#define CN_FILE_SINK_INIT {-1, -1, NULL, NULL, 0, 0, {0}}

int cn_client_name_is_valid(const char *name, size_t max_length);

int cn_manifest_parse(const uint8_t *payload, size_t length,
                      struct cn_manifest *manifest, const char **reason);
void cn_manifest_free(struct cn_manifest *manifest);

int cn_sha256_fd(int fd, uint8_t digest[CN_SHA256_SIZE]);
char *cn_part_name(const char *name);

int cn_client_open_topic_dir(const char *output_dir, const char *topic);
int cn_file_sink_open(struct cn_file_sink *sink, int dir_fd,
                      const struct cn_manifest_entry *entry);
int cn_file_sink_write(struct cn_file_sink *sink, uint64_t offset,
                       const uint8_t *data, size_t length);
enum cn_client_status cn_file_sink_commit(struct cn_file_sink *sink);
void cn_file_sink_abort(struct cn_file_sink *sink);

int cn_client_new_transfer_id(uint8_t transfer_id[CN_TRANSFER_ID_SIZE]);
int cn_client_connect(const char *host, uint16_t port, unsigned timeout_seconds,
                      struct cn_client_report *report);

/*
 * Run one GET transfer on an already connected socket and write the topic to
 * <output_dir>/<topic>/. Progress lines go to log when it is not NULL. The
 * socket is left open; the caller closes it.
 */
enum cn_client_status cn_client_fetch_topic(int socket_fd,
                                            const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                            const char *topic, const char *output_dir,
                                            FILE *log, struct cn_client_report *report);

#endif
