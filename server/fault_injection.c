#define _POSIX_C_SOURCE 200809L

#include "fault_injection.h"

#include "../common/socket_io.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* splitmix64: tiny, fast, good enough for fault decisions, and thread-safe
   because every connection owns its own state word. */
uint64_t cn_fault_next_random(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* Uniform in [0, 1). With p = 0 a fault never fires; with p = 1 it always does. */
double cn_fault_next_unit(uint64_t *state)
{
    return (double)(cn_fault_next_random(state) >> 11) * 0x1.0p-53;
}

static uint64_t next_below(uint64_t *state, uint64_t bound)
{
    return bound == 0 ? 0 : cn_fault_next_random(state) % bound;
}

static int parse_u64(const char *text, uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0') {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

int cn_fault_config_init(struct cn_fault_config *config, double probability,
                         const char **error)
{
    const char *text;
    struct timespec now;

    memset(config, 0, sizeof(*config));
    config->probability = probability;
    config->mode = CN_FAULT_MODE_CONNECTION;
    config->action = CN_FAULT_ACTION_CLOSE;

    text = getenv("CN_FAULT_MODE");
    if (text != NULL && text[0] != '\0') {
        if (strcmp(text, "connection") == 0) {
            config->mode = CN_FAULT_MODE_CONNECTION;
        } else if (strcmp(text, "chunk") == 0) {
            config->mode = CN_FAULT_MODE_CHUNK;
        } else {
            *error = "CN_FAULT_MODE must be 'connection' or 'chunk'";
            return -1;
        }
    }
    text = getenv("CN_FAULT_ACTION");
    if (text != NULL && text[0] != '\0') {
        if (strcmp(text, "close") == 0) {
            config->action = CN_FAULT_ACTION_CLOSE;
        } else if (strcmp(text, "reset") == 0) {
            config->action = CN_FAULT_ACTION_RESET;
        } else {
            *error = "CN_FAULT_ACTION must be 'close' or 'reset'";
            return -1;
        }
    }
    text = getenv("CN_FAULT_SEED");
    if (text != NULL && text[0] != '\0') {
        if (parse_u64(text, &config->seed) != 0) {
            *error = "CN_FAULT_SEED must be an unsigned integer";
            return -1;
        }
        config->seed_from_env = 1;
    } else {
        (void)clock_gettime(CLOCK_REALTIME, &now);
        config->seed = ((uint64_t)now.tv_sec << 32) ^ (uint64_t)now.tv_nsec ^
                       ((uint64_t)getpid() << 16);
    }
    return 0;
}

const char *cn_fault_mode_name(enum cn_fault_mode mode)
{
    return mode == CN_FAULT_MODE_CHUNK ? "chunk" : "connection";
}

const char *cn_fault_action_name(enum cn_fault_action action)
{
    return action == CN_FAULT_ACTION_RESET ? "reset" : "close";
}

void cn_fault_injector_init(struct cn_fault_injector *injector,
                            const struct cn_fault_config *config,
                            uint64_t connection_id)
{
    memset(injector, 0, sizeof(*injector));
    injector->config = *config;
    injector->connection_id = connection_id;
    /* Each connection gets its own stream derived from (seed, connection id),
       so a fixed seed reproduces the same decisions whatever the thread timing. */
    injector->rng_state = config->seed ^ (connection_id * UINT64_C(0xD1B54A32D192ED03));
    (void)cn_fault_next_random(&injector->rng_state);
    injector->cut_at = UINT64_MAX;
}

int cn_fault_plan(struct cn_fault_injector *injector, uint64_t data_bytes)
{
    injector->data_bytes_planned = data_bytes;
    injector->cut_planned = 0;
    injector->cut_at = UINT64_MAX;
    if (injector->config.mode != CN_FAULT_MODE_CONNECTION || data_bytes == 0) {
        return 0;
    }
    if (cn_fault_next_unit(&injector->rng_state) < injector->config.probability) {
        injector->cut_planned = 1;
        injector->cut_at = next_below(&injector->rng_state, data_bytes);
    }
    return injector->cut_planned;
}

static void interrupt_connection(int socket_fd, const struct cn_fault_injector *injector)
{
    if (injector->config.action == CN_FAULT_ACTION_RESET) {
        struct linger hard = {1, 0};
        /* The worker's close() now sends RST instead of FIN. */
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
    } else {
        (void)shutdown(socket_fd, SHUT_RDWR);
    }
}

int cn_fault_send_data_frame(int socket_fd, struct cn_fault_injector *injector,
                             const struct cn_frame_header *header,
                             const uint8_t *payload, size_t data_length)
{
    uint8_t encoded[CN_FRAME_HEADER_SIZE];
    size_t fixed_length, keep = data_length;
    int fire = 0, rc;

    if (header == NULL || payload == NULL || header->payload_length < data_length ||
        cn_encode_header(header, encoded) != CN_PROTOCOL_OK) {
        errno = EINVAL;
        return CN_PROTOCOL_INVALID;
    }
    fixed_length = (size_t)header->payload_length - data_length;

    if (data_length > 0) {
        if (injector->config.mode == CN_FAULT_MODE_CONNECTION) {
            fire = injector->cut_planned && !injector->fired &&
                   injector->cut_at < injector->data_bytes_sent + data_length;
            keep = fire ? (size_t)(injector->cut_at - injector->data_bytes_sent) : data_length;
        } else {
            fire = cn_fault_next_unit(&injector->rng_state) < injector->config.probability;
            keep = fire ? (size_t)next_below(&injector->rng_state, data_length) : data_length;
            if (fire) {
                injector->cut_at = injector->data_bytes_sent + keep;
            }
        }
    }

    if (!fire) {
        rc = cn_send_all(socket_fd, encoded, sizeof(encoded));
        if (rc == CN_PROTOCOL_OK) {
            rc = cn_send_all(socket_fd, payload, (size_t)header->payload_length);
        }
        if (rc == CN_PROTOCOL_OK) {
            injector->data_bytes_sent += data_length;
        }
        return rc;
    }

    /* Send the header, the fixed DATA fields and `keep` data bytes, then cut. */
    rc = cn_send_all(socket_fd, encoded, sizeof(encoded));
    if (rc == CN_PROTOCOL_OK) {
        rc = cn_send_all(socket_fd, payload, fixed_length + keep);
    }
    if (rc == CN_PROTOCOL_OK) {
        injector->data_bytes_sent += keep;
    }
    injector->fired = 1;
    interrupt_connection(socket_fd, injector);
    errno = ECONNABORTED;
    return CN_PROTOCOL_IO_ERROR;
}
