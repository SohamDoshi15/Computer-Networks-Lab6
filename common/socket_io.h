#ifndef CN_SOCKET_IO_H
#define CN_SOCKET_IO_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

int cn_send_all(int socket_fd, const void *buffer, size_t length);
int cn_recv_all(int socket_fd, void *buffer, size_t length);
int cn_send_frame(int socket_fd, const struct cn_frame_header *header,
                  const void *payload);
int cn_recv_frame(int socket_fd, struct cn_frame_header *header,
                  uint8_t **payload_out);

#endif
