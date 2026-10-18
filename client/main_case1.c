#define _POSIX_C_SOURCE 200809L

/*
 * Part 2 Case 1 client: reconnect and restart, no session management.
 *
 * Usage: client <server_ip> <server_port> <topic> <output_dir>
 *
 * Each attempt opens a new connection, generates a new transfer ID and sends
 * a fresh GET, exactly like the Part 1 client. Nothing about an interrupted
 * attempt is kept: its partial file is deleted, and files it had already
 * verified are downloaded and verified again. Only network, protocol and
 * integrity failures (exit codes 3-5) are retried; an unknown topic or a local
 * file system error ends the run at once.
 *
 * Optional environment variables (defaults keep the required CLI unchanged):
 *   CN_MAX_ATTEMPTS     maximum attempts, 0 = unlimited (default 100)
 *   CN_RETRY_DELAY_MS   pause before reconnecting after an interrupted
 *                       transfer (default 50)
 *   CN_METRICS_CSV      append one CSV row of run metrics to this file
 *   CN_QUIET            set to 1 to omit the per-file progress lines
 *   CN_TIMEOUT_SECONDS  per send/receive timeout (default 30)
 *   CN_CONNECT_GIVEUP_SECONDS  keep retrying failed connects this long
 *                       before giving up (default 30), so a short outage
 *                       such as a pulled cable is ridden out
 *
 * On exit the client prints one METRICS line (see docs/FAULT_INJECTION_CASE1.md
 * for the definitions).
 */

#include "cli.h"
#include "client.h"
#include "socket_io.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Default bound on each blocking send and receive. A cut connection is usually
   seen at once (EOF or RST), but a lost RST is only noticed by this timeout. */
#define TIMEOUT_SECONDS 30u
/* Failed connects are retried with exponential backoff until they have kept
   failing for this long; then the server is considered down. */
#define CONNECT_GIVEUP_SECONDS 30u
#define CONNECT_BACKOFF_MAX_MS 2000u

struct run_metrics {
    uint32_t attempts;
    uint32_t files;
    uint64_t wire_bytes_received;   /* every byte from the server, all attempts */
    uint64_t wire_bytes_sent;       /* GET, ACK and ERROR frames, all attempts */
    uint64_t data_bytes_received;   /* file bytes in complete DATA frames, all attempts */
    uint64_t useful_bytes;          /* file bytes delivered and verified */
    uint64_t final_wire_bytes;      /* bytes received by the successful attempt */
    double seconds;
};

static double seconds_since(const struct timespec *start)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

static void sleep_ms(unsigned ms)
{
    struct timespec pause;

    pause.tv_sec = (time_t)(ms / 1000u);
    pause.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

static uint32_t env_u32(const char *name, uint32_t fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return fallback;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

static int is_retryable(enum cn_client_status status)
{
    return status == CN_CLIENT_NETWORK || status == CN_CLIENT_PROTOCOL ||
           status == CN_CLIENT_INTEGRITY;
}

static double ratio(double numerator, double denominator)
{
    return denominator > 0.0 ? numerator / denominator : 0.0;
}

static void print_metrics(FILE *out, const struct run_metrics *m, enum cn_client_status status)
{
    uint64_t redundant = m->data_bytes_received - m->useful_bytes;

    fprintf(out,
            "METRICS status=%d attempts=%" PRIu32 " files=%" PRIu32
            " transmitted_bytes=%" PRIu64 " sent_bytes=%" PRIu64
            " data_bytes_received=%" PRIu64 " useful_bytes=%" PRIu64
            " redundant_bytes=%" PRIu64 " wasted_bytes=%" PRIu64
            " overhead=%.4f completion_s=%.6f goodput_MBps=%.3f throughput_MBps=%.3f\n",
            (int)status, m->attempts, m->files, m->wire_bytes_received, m->wire_bytes_sent,
            m->data_bytes_received, m->useful_bytes, redundant,
            m->wire_bytes_received - m->final_wire_bytes,
            ratio((double)(m->wire_bytes_received - m->useful_bytes), (double)m->useful_bytes),
            m->seconds, ratio((double)m->useful_bytes, m->seconds) / 1e6,
            ratio((double)m->wire_bytes_received, m->seconds) / 1e6);
}

static void append_csv(const char *topic, const struct run_metrics *m,
                       enum cn_client_status status)
{
    const char *path = getenv("CN_METRICS_CSV");
    struct stat st;
    FILE *csv;
    int is_new;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    is_new = stat(path, &st) != 0 || st.st_size == 0;
    csv = fopen(path, "a");
    if (csv == NULL) {
        fprintf(stderr, "client: cannot append metrics to %s: %s\n", path, strerror(errno));
        return;
    }
    if (is_new) {
        fprintf(csv, "topic,status,attempts,files,transmitted_bytes,sent_bytes,"
                     "data_bytes_received,useful_bytes,redundant_bytes,wasted_bytes,"
                     "overhead,completion_s,goodput_MBps,throughput_MBps\n");
    }
    fprintf(csv, "%s,%d,%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                 ",%" PRIu64 ",%" PRIu64 ",%.6f,%.6f,%.6f,%.6f\n",
            topic, (int)status, m->attempts, m->files, m->wire_bytes_received,
            m->wire_bytes_sent, m->data_bytes_received, m->useful_bytes,
            m->data_bytes_received - m->useful_bytes,
            m->wire_bytes_received - m->final_wire_bytes,
            ratio((double)(m->wire_bytes_received - m->useful_bytes), (double)m->useful_bytes),
            m->seconds, ratio((double)m->useful_bytes, m->seconds) / 1e6,
            ratio((double)m->wire_bytes_received, m->seconds) / 1e6);
    fclose(csv);
}

int main(int argc, char **argv)
{
    struct cn_client_report report;
    struct run_metrics metrics;
    struct timespec start;
    enum cn_client_status status = CN_CLIENT_NETWORK;
    uint32_t max_attempts, retry_delay_ms, timeout_seconds, connect_giveup_seconds;
    struct timespec first_connect_failure;
    int connect_failing = 0;
    unsigned backoff_ms;
    uint16_t port;
    FILE *log;

    if (argc != 5 || cn_parse_port(argv[2], &port) != 0 || argv[4][0] == '\0') {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <topic> <output_dir>\n", argv[0]);
        return CN_CLIENT_USAGE;
    }
    if (!cn_client_name_is_valid(argv[3], CN_MAX_TOPIC_SIZE)) {
        fprintf(stderr, "client: invalid topic name: use 1-%u bytes without '/', '\\' or "
                        "control characters, and not '.' or '..'\n", CN_MAX_TOPIC_SIZE);
        return CN_CLIENT_USAGE;
    }
    signal(SIGPIPE, SIG_IGN);
    max_attempts = env_u32("CN_MAX_ATTEMPTS", 100u);
    retry_delay_ms = env_u32("CN_RETRY_DELAY_MS", 50u);
    timeout_seconds = env_u32("CN_TIMEOUT_SECONDS", TIMEOUT_SECONDS);
    connect_giveup_seconds = env_u32("CN_CONNECT_GIVEUP_SECONDS", CONNECT_GIVEUP_SECONDS);
    if (timeout_seconds == 0) {
        timeout_seconds = TIMEOUT_SECONDS;
    }
    log = env_u32("CN_QUIET", 0u) != 0 ? NULL : stdout;
    memset(&metrics, 0, sizeof(metrics));
    memset(&report, 0, sizeof(report));
    backoff_ms = retry_delay_ms > 0 ? retry_delay_ms : 1u;

    /* Completion time runs from the first connect until every file verifies,
       so it includes every failed attempt and every pause between attempts. */
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct cn_io_counters io = {0, 0};
        uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
        int socket_fd;

        if (max_attempts != 0 && metrics.attempts >= max_attempts) {
            fprintf(stderr, "client: giving up after %" PRIu32 " attempt(s)\n", metrics.attempts);
            break;
        }
        metrics.attempts += 1;
        memset(&report, 0, sizeof(report));

        socket_fd = cn_client_connect(argv[1], port, timeout_seconds, &report);
        if (socket_fd < 0) {
            status = report.status;
            if (!connect_failing) {
                connect_failing = 1;
                (void)clock_gettime(CLOCK_MONOTONIC, &first_connect_failure);
            }
            fprintf(stderr, "client: attempt %" PRIu32 ": %s\n", metrics.attempts, report.message);
            if (seconds_since(&first_connect_failure) >= (double)connect_giveup_seconds) {
                fprintf(stderr, "client: server unreachable for %" PRIu32 " s, giving up\n",
                        connect_giveup_seconds);
                break;
            }
            sleep_ms(backoff_ms);
            backoff_ms = backoff_ms * 2u > CONNECT_BACKOFF_MAX_MS ? CONNECT_BACKOFF_MAX_MS
                                                                : backoff_ms * 2u;
            continue;
        }
        connect_failing = 0;
        backoff_ms = retry_delay_ms > 0 ? retry_delay_ms : 1u;

        /* A new transfer ID per attempt: the server sees an unrelated request. */
        if (cn_client_new_transfer_id(transfer_id) != 0) {
            perror("client: cannot generate a transfer ID");
            close(socket_fd);
            status = CN_CLIENT_LOCAL_IO;
            break;
        }
        cn_io_set_counters(&io);
        status = cn_client_fetch_topic(socket_fd, transfer_id, argv[3], argv[4], log, &report);
        cn_io_set_counters(NULL);
        close(socket_fd);

        metrics.wire_bytes_received += io.bytes_received;
        metrics.wire_bytes_sent += io.bytes_sent;
        metrics.data_bytes_received += report.data_bytes_received;
        if (status == CN_CLIENT_OK) {
            metrics.useful_bytes = report.bytes_completed;
            metrics.files = report.files_completed;
            metrics.final_wire_bytes = io.bytes_received;
            break;
        }
        fflush(stdout);
        fprintf(stderr, "client: attempt %" PRIu32 " failed after %" PRIu64
                        " data byte(s): %s\n", metrics.attempts, report.data_bytes_received,
                report.message);
        if (!is_retryable(status)) {
            break;
        }
        fprintf(stderr, "client: reconnecting and restarting %s from the beginning\n", argv[3]);
        if (retry_delay_ms > 0) {
            sleep_ms(retry_delay_ms);
        }
    }
    metrics.seconds = seconds_since(&start);

    if (status == CN_CLIENT_OK) {
        printf("Received %" PRIu32 " file(s), %" PRIu64 " verified byte(s) into %s/%s in %.3f s "
               "(%.2f MB/s) after %" PRIu32 " attempt(s)\n", metrics.files, metrics.useful_bytes,
               argv[4], argv[3], metrics.seconds,
               ratio((double)metrics.useful_bytes, metrics.seconds) / 1e6, metrics.attempts);
    }
    print_metrics(stdout, &metrics, status);
    fflush(stdout);
    append_csv(argv[3], &metrics, status);
    return status == CN_CLIENT_OK ? EXIT_SUCCESS : (int)status;
}
