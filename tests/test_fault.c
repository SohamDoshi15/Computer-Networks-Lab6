/* Unit tests for server/fault_injection.c: decisions, cut placement, byte accounting. */

#define _POSIX_C_SOURCE 200809L /* setenv and unsetenv under -std=c11 on glibc */

#include "protocol.h"
#include "serialization.h"
#include "socket_io.h"
#include "../server/fault_injection.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DATA_FIXED 16u   /* file_index u32, offset u64, data_length u32 */
#define CHUNK 512u       /* small enough to fit a socketpair buffer */

static struct cn_fault_config make_config(double p, enum cn_fault_mode mode, uint64_t seed)
{
    struct cn_fault_config config;
    memset(&config, 0, sizeof(config));
    config.probability = p;
    config.mode = mode;
    config.action = CN_FAULT_ACTION_CLOSE;
    config.seed = seed;
    config.seed_from_env = 1;
    return config;
}

static size_t build_data(uint8_t *payload, uint64_t offset, uint32_t length)
{
    struct cn_writer writer;
    uint32_t i;
    cn_writer_init(&writer, payload, DATA_FIXED + length);
    assert(cn_writer_put_u32(&writer, 0) == 0);
    assert(cn_writer_put_u64(&writer, offset) == 0);
    assert(cn_writer_put_u32(&writer, length) == 0);
    for (i = 0; i < length; ++i) {
        assert(cn_writer_put_u8(&writer, (uint8_t)(offset + i)) == 0);
    }
    return writer.offset;
}

/* Reads whatever is available until EOF or `limit` bytes; returns the count. */
static size_t drain(int fd, size_t limit)
{
    uint8_t buffer[4096];
    size_t total = 0;
    while (total < limit) {
        size_t want = limit - total < sizeof(buffer) ? limit - total : sizeof(buffer);
        ssize_t n = recv(fd, buffer, want, 0);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

/*
 * Streams `data_bytes` of file data in CHUNK-sized DATA frames through the
 * injector. Returns the number of frames sent completely; *fired tells whether
 * the fault fired and *peer_bytes how many bytes the peer received in total.
 */
static size_t stream(struct cn_fault_injector *injector, uint64_t data_bytes,
                     int *fired, size_t *peer_bytes, struct cn_io_counters *io)
{
    int fds[2];
    uint8_t payload[DATA_FIXED + CHUNK];
    uint64_t offset = 0;
    size_t frames = 0;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    *fired = 0;
    *peer_bytes = 0;
    cn_io_set_counters(io);
    (void)cn_fault_plan(injector, data_bytes);
    while (offset < data_bytes) {
        uint32_t length = data_bytes - offset < CHUNK ? (uint32_t)(data_bytes - offset) : CHUNK;
        struct cn_frame_header header;
        size_t payload_length = build_data(payload, offset, length);
        int rc;

        memset(&header, 0, sizeof(header));
        header.version = CN_PROTOCOL_VERSION;
        header.message_type = CN_MSG_DATA;
        header.payload_length = payload_length;
        rc = cn_fault_send_data_frame(fds[0], injector, &header, payload, length);
        if (rc != CN_PROTOCOL_OK) {
            assert(rc == CN_PROTOCOL_IO_ERROR && errno == ECONNABORTED);
            assert(injector->fired);
            *fired = 1;
            /* After the cut the peer sees the partial frame, then EOF. */
            *peer_bytes += drain(fds[1], SIZE_MAX);
            break;
        }
        *peer_bytes += drain(fds[1], CN_FRAME_HEADER_SIZE + payload_length);
        offset += length;
        frames += 1;
    }
    cn_io_set_counters(NULL);
    close(fds[0]);
    close(fds[1]);
    return frames;
}

static void test_random_is_deterministic(void)
{
    uint64_t a = 42, b = 42;
    int i;
    for (i = 0; i < 1000; ++i) {
        double u = cn_fault_next_unit(&a);
        assert(u >= 0.0 && u < 1.0);
        assert(cn_fault_next_random(&b) >> 11 == (uint64_t)(u * 0x1.0p53));
    }
}

static void test_probability_zero_never_fires(void)
{
    struct cn_fault_config config;
    struct cn_fault_injector injector;
    struct cn_io_counters io = {0, 0};
    uint64_t id;
    int fired;
    size_t peer;
    const uint64_t data = 10 * CHUNK + 7;
    const size_t frames = 11;

    config = make_config(0.0, CN_FAULT_MODE_CONNECTION, 1);
    for (id = 1; id <= 200; ++id) {
        cn_fault_injector_init(&injector, &config, id);
        assert(cn_fault_plan(&injector, 1000000) == 0);
    }
    cn_fault_injector_init(&injector, &config, 1);
    assert(stream(&injector, data, &fired, &peer, &io) == frames);
    assert(!fired && injector.data_bytes_sent == data);
    assert(peer == frames * (CN_FRAME_HEADER_SIZE + DATA_FIXED) + data);
    assert(io.bytes_sent == peer);

    config = make_config(0.0, CN_FAULT_MODE_CHUNK, 1);
    cn_fault_injector_init(&injector, &config, 1);
    assert(stream(&injector, data, &fired, &peer, &io) == frames && !fired);
}

static void test_connection_mode_cut_is_exact(void)
{
    struct cn_fault_config config = make_config(1.0, CN_FAULT_MODE_CONNECTION, 99);
    struct cn_fault_injector injector;
    uint64_t id;

    for (id = 1; id <= 300; ++id) {
        struct cn_io_counters io = {0, 0};
        const uint64_t data = 20 * CHUNK + 100;
        size_t frames, peer;
        int fired;

        cn_fault_injector_init(&injector, &config, id);
        frames = stream(&injector, data, &fired, &peer, &io);
        assert(fired && injector.cut_planned);
        assert(injector.cut_at < data);
        /* Exactly cut_at data bytes left the server, then the frame was cut. */
        assert(injector.data_bytes_sent == injector.cut_at);
        assert(frames == injector.cut_at / CHUNK);
        assert(peer == (frames + 1) * (CN_FRAME_HEADER_SIZE + DATA_FIXED) + injector.cut_at);
        /* The byte counters saw exactly what the peer received. */
        assert(io.bytes_sent == peer);
    }
}

static void test_chunk_mode_fires_in_first_frame_at_p1(void)
{
    struct cn_fault_config config = make_config(1.0, CN_FAULT_MODE_CHUNK, 5);
    struct cn_fault_injector injector;
    struct cn_io_counters io = {0, 0};
    size_t peer;
    int fired;

    cn_fault_injector_init(&injector, &config, 1);
    assert(stream(&injector, 5 * CHUNK, &fired, &peer, &io) == 0);
    assert(fired && injector.cut_at < CHUNK && injector.data_bytes_sent == injector.cut_at);
    assert(peer == CN_FRAME_HEADER_SIZE + DATA_FIXED + injector.cut_at);
}

static void test_empty_transfer_is_never_cut(void)
{
    struct cn_fault_config config = make_config(1.0, CN_FAULT_MODE_CONNECTION, 5);
    struct cn_fault_injector injector;
    cn_fault_injector_init(&injector, &config, 1);
    assert(cn_fault_plan(&injector, 0) == 0);
}

static void test_same_seed_same_decisions(void)
{
    struct cn_fault_config config = make_config(0.5, CN_FAULT_MODE_CONNECTION, 1234);
    struct cn_fault_injector a, b;
    uint64_t id;
    int differs = 0;

    for (id = 1; id <= 100; ++id) {
        cn_fault_injector_init(&a, &config, id);
        cn_fault_injector_init(&b, &config, id);
        assert(cn_fault_plan(&a, 1u << 30) == cn_fault_plan(&b, 1u << 30));
        assert(a.cut_at == b.cut_at);
    }
    /* A different seed gives a different sequence. */
    for (id = 1; id <= 100; ++id) {
        struct cn_fault_config other = make_config(0.5, CN_FAULT_MODE_CONNECTION, 4321);
        cn_fault_injector_init(&a, &config, id);
        cn_fault_injector_init(&b, &other, id);
        differs |= cn_fault_plan(&a, 1u << 30) != cn_fault_plan(&b, 1u << 30) ||
                   a.cut_at != b.cut_at;
    }
    assert(differs);
}

/* The observed failure rate and cut position match the configured distribution. */
static void test_decision_statistics(void)
{
    const double probabilities[] = {0.1, 0.3, 0.5, 0.9};
    const uint64_t trials = 20000, data = 1000000;
    size_t k;

    for (k = 0; k < sizeof(probabilities) / sizeof(probabilities[0]); ++k) {
        struct cn_fault_config config = make_config(probabilities[k], CN_FAULT_MODE_CONNECTION, 77);
        struct cn_fault_injector injector;
        uint64_t id, failures = 0;
        double cut_sum = 0.0, rate, mean_cut;

        for (id = 1; id <= trials; ++id) {
            cn_fault_injector_init(&injector, &config, id);
            if (cn_fault_plan(&injector, data)) {
                failures += 1;
                cut_sum += (double)injector.cut_at;
            }
        }
        rate = (double)failures / (double)trials;
        mean_cut = cut_sum / (double)failures;
        assert(rate > probabilities[k] - 0.02 && rate < probabilities[k] + 0.02);
        assert(mean_cut > 0.45 * (double)data && mean_cut < 0.55 * (double)data);
    }
}

static void test_config_from_environment(void)
{
    struct cn_fault_config config;
    const char *error = NULL;

    unsetenv("CN_FAULT_MODE");
    unsetenv("CN_FAULT_ACTION");
    unsetenv("CN_FAULT_SEED");
    assert(cn_fault_config_init(&config, 0.25, &error) == 0);
    assert(config.probability == 0.25 && config.mode == CN_FAULT_MODE_CONNECTION &&
           config.action == CN_FAULT_ACTION_CLOSE && !config.seed_from_env);

    setenv("CN_FAULT_MODE", "chunk", 1);
    setenv("CN_FAULT_ACTION", "reset", 1);
    setenv("CN_FAULT_SEED", "18446744073709551615", 1);
    assert(cn_fault_config_init(&config, 1.0, &error) == 0);
    assert(config.mode == CN_FAULT_MODE_CHUNK && config.action == CN_FAULT_ACTION_RESET &&
           config.seed == UINT64_MAX && config.seed_from_env);

    setenv("CN_FAULT_MODE", "sometimes", 1);
    assert(cn_fault_config_init(&config, 1.0, &error) == -1 && error != NULL);
    setenv("CN_FAULT_MODE", "connection", 1);
    setenv("CN_FAULT_ACTION", "explode", 1);
    assert(cn_fault_config_init(&config, 1.0, &error) == -1);
    setenv("CN_FAULT_ACTION", "close", 1);
    setenv("CN_FAULT_SEED", "-3", 1);
    assert(cn_fault_config_init(&config, 1.0, &error) == -1);
    setenv("CN_FAULT_SEED", "12abc", 1);
    assert(cn_fault_config_init(&config, 1.0, &error) == -1);
    unsetenv("CN_FAULT_MODE");
    unsetenv("CN_FAULT_ACTION");
    unsetenv("CN_FAULT_SEED");
}

int main(void)
{
    test_random_is_deterministic();
    test_probability_zero_never_fires();
    test_connection_mode_cut_is_exact();
    test_chunk_mode_fires_in_first_frame_at_p1();
    test_empty_transfer_is_never_cut();
    test_same_seed_same_decisions();
    test_decision_statistics();
    test_config_from_environment();
    puts("all fault injection tests passed");
    return 0;
}
