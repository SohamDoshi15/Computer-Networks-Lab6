#include "socket_io.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int cn_send_all(int socket_fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;
    size_t sent = 0;

    if (buffer == NULL && length != 0) {
        errno = EINVAL;
        return CN_PROTOCOL_INVALID;
    }

    while (sent < length) {
        const ssize_t result = send(socket_fd, cursor + sent, length - sent,
                                    MSG_NOSIGNAL);
        if (result > 0) {
            sent += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            errno = EPIPE;
        }
        return CN_PROTOCOL_IO_ERROR;
    }
    return CN_PROTOCOL_OK;
}

int cn_recv_all(int socket_fd, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    size_t received = 0;

    if (buffer == NULL && length != 0) {
        errno = EINVAL;
        return CN_PROTOCOL_INVALID;
    }

    while (received < length) {
        const ssize_t result = recv(socket_fd, cursor + received,
                                    length - received, 0);
        if (result > 0) {
            received += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result == 0) {
            return received == 0 ? CN_PROTOCOL_EOF : CN_PROTOCOL_TRUNCATED;
        }
        return CN_PROTOCOL_IO_ERROR;
    }
    return CN_PROTOCOL_OK;
}

int cn_send_frame(int socket_fd, const struct cn_frame_header *header,
                  const void *payload)
{
    uint8_t encoded_header[CN_FRAME_HEADER_SIZE];
    int result;

    if ((header != NULL && header->payload_length != 0 && payload == NULL) ||
        cn_encode_header(header, encoded_header) != CN_PROTOCOL_OK) {
        return CN_PROTOCOL_INVALID;
    }

    result = cn_send_all(socket_fd, encoded_header, sizeof(encoded_header));
    if (result != CN_PROTOCOL_OK || header->payload_length == 0) {
        return result;
    }
    return cn_send_all(socket_fd, payload, (size_t)header->payload_length);
}

int cn_recv_frame(int socket_fd, struct cn_frame_header *header,
                  uint8_t **payload_out)
{
    uint8_t encoded_header[CN_FRAME_HEADER_SIZE];
    uint8_t *payload = NULL;
    int result;

    if (header == NULL || payload_out == NULL) {
        return CN_PROTOCOL_INVALID;
    }
    *payload_out = NULL;

    result = cn_recv_all(socket_fd, encoded_header, sizeof(encoded_header));
    if (result != CN_PROTOCOL_OK) {
        return result;
    }
    result = cn_decode_header(encoded_header, header);
    if (result != CN_PROTOCOL_OK || header->payload_length == 0) {
        return result;
    }

    payload = malloc((size_t)header->payload_length);
    if (payload == NULL) {
        return CN_PROTOCOL_NO_MEMORY;
    }
    result = cn_recv_all(socket_fd, payload, (size_t)header->payload_length);
    if (result != CN_PROTOCOL_OK) {
        free(payload);
        return result;
    }
    *payload_out = payload;
    return CN_PROTOCOL_OK;
}
