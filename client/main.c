#define _POSIX_C_SOURCE 200809L

#include "cli.h"
#include "client.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_SECONDS 30u

int main(int argc, char **argv)
{
    struct cn_client_report report;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    uint32_t timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
    enum cn_client_status status;
    uint16_t port;
    int socket_fd;

    if ((argc != 5 && argc != 6) || cn_parse_port(argv[2], &port) != 0 ||
        argv[4][0] == '\0' ||
        (argc == 6 && cn_parse_positive_u32(argv[5], &timeout_seconds) != 0)) {
        fprintf(stderr, "Usage: %s <server_host> <port> <topic> <output_dir> [timeout_seconds]\n",
                argv[0]);
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
    socket_fd = cn_client_connect(argv[1], port, timeout_seconds, &report);
    if (socket_fd < 0) {
        fprintf(stderr, "client: %s\n", report.message);
        return report.status;
    }

    status = cn_client_fetch_topic(socket_fd, transfer_id, argv[3], argv[4], stdout, &report);
    close(socket_fd);
    if (status != CN_CLIENT_OK) {
        fflush(stdout);
        fprintf(stderr, "client: %s\n", report.message);
        return status;
    }
    printf("Received %" PRIu32 " file(s), %" PRIu64 " byte(s) into %s/%s\n",
           report.files_completed, report.bytes_completed, argv[4], argv[3]);
    return EXIT_SUCCESS;
}
