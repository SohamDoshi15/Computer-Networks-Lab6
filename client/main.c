#define _POSIX_C_SOURCE 200809L

#include "cli.h"
#include "client.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bounds each blocking send and receive so a stalled server cannot hang the client. */
#define TIMEOUT_SECONDS 30u

static double seconds_since(const struct timespec *start)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
    struct cn_client_report report;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    struct timespec start;
    enum cn_client_status status;
    double elapsed;
    uint16_t port;
    int socket_fd;

    /* Trailing arguments stay free for case-specific settings such as the
       Enhanced Case 2 connection count. */
    if (argc != 5 || cn_parse_port(argv[2], &port) != 0 || argv[4][0] == '\0') {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir>\n", argv[0]);
        return CN_CLIENT_USAGE;
    }
    /* Reject a bad topic before touching the network. */
    if (!cn_client_name_is_valid(argv[3], CN_MAX_TOPIC_SIZE)) {
        fprintf(stderr, "client: invalid topic name: use 1-%u bytes without '/', '\\' or "
                        "control characters, and not '.' or '..'\n", CN_MAX_TOPIC_SIZE);
        return CN_CLIENT_USAGE;
    }
    signal(SIGPIPE, SIG_IGN);

    memset(&report, 0, sizeof(report));
    if (cn_client_new_transfer_id(transfer_id) != 0) {
        perror("client: cannot generate a transfer ID");
        return CN_CLIENT_LOCAL_IO;
    }
    /* Completion time runs from the first request until every file verifies. */
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    socket_fd = cn_client_connect(argv[1], port, TIMEOUT_SECONDS, &report);
    if (socket_fd < 0) {
        fprintf(stderr, "client: %s\n", report.message);
        return report.status;
    }

    status = cn_client_fetch_topic(socket_fd, transfer_id, argv[3], argv[4], stdout, &report);
    elapsed = seconds_since(&start);
    close(socket_fd);
    if (status != CN_CLIENT_OK) {
        fflush(stdout);
        fprintf(stderr, "client: %s\n", report.message);
        return status;
    }
    printf("Received %" PRIu32 " file(s), %" PRIu64 " verified byte(s) into %s/%s in %.3f s "
           "(%.2f MB/s)\n", report.files_completed, report.bytes_completed, argv[4], argv[3],
           elapsed, elapsed > 0.0 ? (double)report.bytes_completed / elapsed / 1e6 : 0.0);
    return EXIT_SUCCESS;
}
