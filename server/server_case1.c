#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 1 server: thread-per-client, no session management, with fault
 * injection. The server keeps nothing about a transfer once its connection
 * ends, so a client that is cut off simply sends a new GET and gets the whole
 * topic again. See fault_injection.h for when and how failures are decided.
 *
 * Usage: server <port> <topics_root_dir> <failure_probability>
 *
 * One line per finished connection is printed to stdout. Setting
 * CN_SERVER_METRICS_CSV=<path> also appends that line to a CSV file.
 */

#include "fault_injection.h"
#include "server_common.h"
#include "../common/cli.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

struct worker_args {
    int client_fd;
    uint64_t connection_id;
    const char *root;
    const struct cn_fault_config *fault_config;
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *metrics_csv = NULL;

static const char *outcome(const struct cn_serve_stats *stats)
{
    if (stats->completed) return "complete";
    if (stats->fault_fired) return "injected_fault";
    return "failed";
}

static void record_connection(const struct worker_args *args,
                              const struct cn_fault_injector *fault,
                              const struct cn_serve_stats *stats)
{
    const char *topic = stats->topic[0] != '\0' ? stats->topic : "-";
    double mbps = stats->seconds > 0.0 ? (double)stats->io.bytes_sent / stats->seconds / 1e6 : 0.0;

    pthread_mutex_lock(&log_lock);
    printf("conn=%" PRIu64 " topic=%s result=%s files_acked=%" PRIu32 "/%" PRIu32
           " data_sent=%" PRIu64 "/%" PRIu64 " wire_sent=%" PRIu64 " wire_received=%" PRIu64
           " seconds=%.6f send_rate=%.2fMB/s",
           args->connection_id, topic, outcome(stats), stats->files_acked, stats->file_count,
           stats->data_bytes_sent, stats->data_bytes_planned, stats->io.bytes_sent,
           stats->io.bytes_received, stats->seconds, mbps);
    if (fault->fired) {
        printf(" cut_at_data_byte=%" PRIu64, fault->cut_at);
    }
    printf("\n");
    fflush(stdout);
    if (metrics_csv != NULL) {
        fprintf(metrics_csv,
                "%" PRIu64 ",%s,%s,%.4f,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%.6f,%d,%" PRIu64 "\n",
                args->connection_id, topic, outcome(stats), args->fault_config->probability,
                cn_fault_mode_name(args->fault_config->mode), stats->files_acked,
                stats->file_count, stats->data_bytes_sent, stats->data_bytes_planned,
                stats->io.bytes_sent, stats->io.bytes_received, stats->seconds, fault->fired,
                fault->fired ? fault->cut_at : 0);
        fflush(metrics_csv);
    }
    pthread_mutex_unlock(&log_lock);
}

static void *serve_worker(void *opaque)
{
    struct worker_args *args = opaque;
    struct cn_fault_injector fault;
    struct cn_serve_stats stats;

    /* Fresh state for every connection: nothing survives a disconnect (Case 1). */
    cn_fault_injector_init(&fault, args->fault_config, args->connection_id);
    (void)part1_serve_client_ex(args->client_fd, args->root, &fault, &stats);
    close(args->client_fd);
    record_connection(args, &fault, &stats);
    free(args);
    return NULL;
}

static int open_metrics_csv(void)
{
    const char *path = getenv("CN_SERVER_METRICS_CSV");
    struct stat st;
    int is_new;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    is_new = stat(path, &st) != 0 || st.st_size == 0;
    metrics_csv = fopen(path, "a");
    if (metrics_csv == NULL) {
        perror(path);
        return -1;
    }
    if (is_new) {
        fprintf(metrics_csv, "connection_id,topic,result,failure_probability,fault_mode,"
                             "files_acked,file_count,data_bytes_sent,data_bytes_planned,"
                             "wire_bytes_sent,wire_bytes_received,seconds,fault_fired,"
                             "cut_at_data_byte\n");
        fflush(metrics_csv);
    }
    return 0;
}

int main(int argc, char **argv)
{
    static struct cn_fault_config fault_config;
    uint16_t port;
    double probability;
    int server_fd, reuse = 1;
    uint64_t next_connection_id = 1;
    struct sockaddr_in address;
    const char *error = NULL;
    DIR *root;

    if (argc != 4 || cn_parse_port(argv[1], &port) != 0 ||
        cn_parse_probability(argv[3], &probability) != 0) {
        fprintf(stderr, "Usage: %s <port> <topics_root_dir> <failure_probability 0..1>\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    root = opendir(argv[2]);
    if (root == NULL) {
        fprintf(stderr, "%s: cannot open topics root %s: %s\n", argv[0], argv[2],
                strerror(errno));
        return EXIT_FAILURE;
    }
    closedir(root);
    if (cn_fault_config_init(&fault_config, probability, &error) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], error);
        return EXIT_FAILURE;
    }
    if (open_metrics_csv() != 0) {
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
        listen(server_fd, 128) != 0) {
        perror("bind/listen");
        close(server_fd);
        return EXIT_FAILURE;
    }
    printf("Part 2 Case 1 server listening on port %u (failure probability %.4f, "
           "mode %s, action %s, seed %" PRIu64 "%s)\n",
           (unsigned)port, fault_config.probability, cn_fault_mode_name(fault_config.mode),
           cn_fault_action_name(fault_config.action), fault_config.seed,
           fault_config.seed_from_env ? "" : " from clock");
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        pthread_t thread;
        struct worker_args *args;
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        args = malloc(sizeof(*args));
        if (args == NULL) { close(client_fd); continue; }
        args->client_fd = client_fd;
        args->connection_id = next_connection_id++;
        args->root = argv[2];
        args->fault_config = &fault_config;
        if (pthread_create(&thread, NULL, serve_worker, args) != 0) {
            close(client_fd);
            free(args);
            continue;
        }
        (void)pthread_detach(thread);
    }
}
