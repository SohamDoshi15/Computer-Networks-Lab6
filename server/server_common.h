#ifndef PART1_SERVER_COMMON_H
#define PART1_SERVER_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../common/protocol.h"
#include "../common/socket_io.h"

struct cn_fault_injector;

struct part1_file {
    char *name;
    uint64_t size;
    uint8_t sha256[32];
};

struct part1_manifest {
    char *topic_path;
    struct part1_file *files;
    size_t count;
    uint64_t total_bytes;
};

/* Per send/recv limit on client sockets, matching the client's default. */
#define CN_SERVER_IO_TIMEOUT_SECONDS 30u

/*
 * Bounds every blocking send and recv on a client socket, so a silent or
 * stalled client cannot hold a worker thread forever (or, in the
 * single-threaded server, every other client).
 */
int part1_set_io_timeout(int client_fd, unsigned seconds);

int part1_validate_topic_name(const char *topic);
int part1_build_manifest(const char *root, const char *topic,
                         struct part1_manifest *manifest);
void part1_free_manifest(struct part1_manifest *manifest);
int part1_send_error(int client_fd, const uint8_t transfer_id[16],
                     uint32_t code, const char *message);
int part1_serve_client(int client_fd, const char *root);

/* What one connection did, filled in by part1_serve_client_ex(). */
struct cn_serve_stats {
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint32_t file_count;          /* files in the manifest */
    uint32_t files_acked;         /* files the client verified and acknowledged */
    uint64_t data_bytes_planned;  /* file bytes this connection should carry */
    uint64_t data_bytes_sent;     /* file bytes actually put on the socket */
    struct cn_io_counters io;     /* every byte sent and received, headers included */
    double seconds;               /* request received until the connection ended */
    int fault_fired;
    int completed;                /* COMPLETE was sent */
};

/*
 * part1_serve_client() with optional fault injection and statistics; either
 * pointer may be NULL. The Part 1 servers pass NULL for both.
 */
int part1_serve_client_ex(int client_fd, const char *root,
                          struct cn_fault_injector *fault,
                          struct cn_serve_stats *stats);

#endif
