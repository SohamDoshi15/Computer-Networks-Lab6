#ifndef CN_FAULT_INJECTION_H
#define CN_FAULT_INJECTION_H

/*
 * Server-side fault injection for Part 2.
 *
 * The server interrupts an active client connection while it is sending file
 * data. The failure probability p comes from the server's third command-line
 * argument; everything else has a default and can be changed with environment
 * variables, so the required invocation `./server <port> <root> <p>` always
 * works:
 *
 *   CN_FAULT_MODE    connection (default) | chunk
 *   CN_FAULT_ACTION  close (default) | reset
 *   CN_FAULT_SEED    unsigned integer; fixes the random sequence for repeatable runs
 *
 * When and how the failure decision is made:
 *
 *   connection  One Bernoulli(p) trial per connection, made right after the
 *               MANIFEST has been sent, when the server knows how many data
 *               bytes D it is about to send on this connection. On failure, a
 *               cut point is drawn uniformly from [0, D): the connection is
 *               interrupted after exactly that many file-data bytes. So p is
 *               the probability that a connection fails, and the failure point
 *               is uniform over the transfer.
 *   chunk       One Bernoulli(p) trial before every DATA frame (up to 64 KiB of
 *               file data). On failure the cut point is drawn uniformly inside
 *               that frame's data. So p is a per-chunk hazard, and a bigger
 *               topic is more likely to be interrupted.
 *
 * In both modes the cut happens in the middle of a DATA frame: the frame
 * header, the DATA fixed fields, and the first k data bytes are sent, and then
 * the connection is terminated. The client therefore sees a truncated frame,
 * the hardest case for its receive path. Topics with no file data are never
 * interrupted, because there is no transfer in progress to interrupt.
 *
 *   close  shutdown(SHUT_RDWR): the peer receives every byte sent so far, then EOF.
 *   reset  SO_LINGER {on, 0}: the worker's close() sends a TCP RST, so unread
 *          bytes may be discarded by the peer, as after a crash. TCP does not
 *          guarantee the RST arrives (for example, a peer whose receive window
 *          is full may drop it), so the client may detect this failure only
 *          through its receive timeout, as with a real crashed host.
 */

#include "../common/protocol.h"

#include <stddef.h>
#include <stdint.h>

enum cn_fault_mode {
    CN_FAULT_MODE_CONNECTION = 0,
    CN_FAULT_MODE_CHUNK = 1
};

enum cn_fault_action {
    CN_FAULT_ACTION_CLOSE = 0,
    CN_FAULT_ACTION_RESET = 1
};

struct cn_fault_config {
    double probability;
    enum cn_fault_mode mode;
    enum cn_fault_action action;
    uint64_t seed;
    int seed_from_env;
};

/* Per-connection state. Each worker thread owns its injector, so no locking. */
struct cn_fault_injector {
    struct cn_fault_config config;
    uint64_t connection_id;
    uint64_t rng_state;
    uint64_t data_bytes_planned;
    uint64_t data_bytes_sent;  /* file-data bytes handed to the socket */
    uint64_t cut_at;           /* connection mode: data byte where the cut happens */
    int cut_planned;
    int fired;
};

/*
 * Fills config from probability and the CN_FAULT_* environment variables.
 * Returns 0, or -1 with *error describing the invalid variable.
 */
int cn_fault_config_init(struct cn_fault_config *config, double probability,
                         const char **error);
const char *cn_fault_mode_name(enum cn_fault_mode mode);
const char *cn_fault_action_name(enum cn_fault_action action);

void cn_fault_injector_init(struct cn_fault_injector *injector,
                            const struct cn_fault_config *config,
                            uint64_t connection_id);

/*
 * Called once the connection's data volume is known (after MANIFEST). In
 * connection mode this is where the failure decision is made. Returns 1 when
 * a cut is planned for this connection, 0 otherwise.
 */
int cn_fault_plan(struct cn_fault_injector *injector, uint64_t data_bytes);

/*
 * Sends one DATA frame, possibly interrupting the connection inside it. The
 * file data is the last data_length bytes of payload. Returns CN_PROTOCOL_OK
 * when the whole frame was sent; when the fault fires it returns
 * CN_PROTOCOL_IO_ERROR with errno set to ECONNABORTED and injector->fired set.
 */
int cn_fault_send_data_frame(int socket_fd, struct cn_fault_injector *injector,
                             const struct cn_frame_header *header,
                             const uint8_t *payload, size_t data_length);

/* Deterministic generator used for decisions; exposed for tests. */
uint64_t cn_fault_next_random(uint64_t *state);
double cn_fault_next_unit(uint64_t *state);

#endif
