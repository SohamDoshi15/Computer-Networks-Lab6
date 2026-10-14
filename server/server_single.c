#define _POSIX_C_SOURCE 200809L

#include "server_common.h"
#include "../common/cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    uint16_t port;
    int server_fd, reuse = 1;
    struct sockaddr_in address;

    if (argc != 3 || cn_parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return EXIT_FAILURE; }
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_fd, 32) != 0) {
        perror("bind/listen");
        close(server_fd);
        return EXIT_FAILURE;
    }
    printf("Part 1 single-threaded server listening on port %u\n", (unsigned)port);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        (void)part1_serve_client(client_fd, argv[2]);
        close(client_fd);
    }
}
