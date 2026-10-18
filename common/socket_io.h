#ifndef CN_SOCKET_IO_H
#define CN_SOCKET_IO_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Byte accounting for performance measurement. Every byte that cn_send_all()
 * and cn_recv_all() move on the socket, including the bytes of a frame that
 * is cut short by a disconnect, is added to the counters installed for the
 * calling thread. Counting is off (NULL) by default.
 */
struct cn_io_counters {
    uint64_t bytes_sent;
    uint64_t bytes_received;
};

void cn_io_set_counters(struct cn_io_counters *counters);
struct cn_io_counters *cn_io_get_counters(void);

/*
 * Disables Nagle's algorithm. Frames are written as a header followed by a
 * payload, and every file ends in a request/response turn (FILE_END, then
 * ACK). With Nagle on, the second small write waits for the peer's delayed
 * ACK, about 40 ms on Linux, once per file. Every connected socket calls this.
 */
int cn_socket_set_nodelay(int socket_fd);

int cn_send_all(int socket_fd, const void *buffer, size_t length);
int cn_recv_all(int socket_fd, void *buffer, size_t length);
int cn_send_frame(int socket_fd, const struct cn_frame_header *header,
                  const void *payload);
int cn_recv_frame(int socket_fd, struct cn_frame_header *header,
                  uint8_t **payload_out);

#endif
