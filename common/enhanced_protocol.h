#ifndef CN_ENHANCED_PROTOCOL_H
#define CN_ENHANCED_PROTOCOL_H

#include "protocol.h"
#include <stddef.h>
#include <stdint.h>

#define CN_ASSIGN_RANGE_PAYLOAD_SIZE 24u
#define CN_RANGE_READY_PAYLOAD_SIZE  24u
#define CN_ACK_RANGE_PAYLOAD_SIZE    16u
#define CN_RANGE_END_PAYLOAD_SIZE    16u

struct cn_range_assignment {
    uint32_t range_id;
    uint32_t file_index;
    uint64_t start_offset;
    uint64_t end_offset;
};

struct cn_range_ack {
    uint32_t range_id;
    uint32_t file_index;
    uint64_t contiguous_offset;
};

struct cn_range_end {
    uint32_t range_id;
    uint32_t file_index;
    uint64_t end_offset;
};

/* ASSIGN_RANGE / RANGE_READY payload encoding & decoding */
int cn_encode_assign_range(const struct cn_range_assignment *range,
                           uint8_t output[CN_ASSIGN_RANGE_PAYLOAD_SIZE]);
int cn_decode_assign_range(const uint8_t *payload, size_t length,
                           struct cn_range_assignment *range);

int cn_encode_range_ready(const struct cn_range_assignment *range,
                          uint8_t output[CN_RANGE_READY_PAYLOAD_SIZE]);
int cn_decode_range_ready(const uint8_t *payload, size_t length,
                          struct cn_range_assignment *range);

/* ACK_RANGE payload encoding & decoding */
int cn_encode_ack_range(const struct cn_range_ack *ack,
                        uint8_t output[CN_ACK_RANGE_PAYLOAD_SIZE]);
int cn_decode_ack_range(const uint8_t *payload, size_t length,
                        struct cn_range_ack *ack);

/* RANGE_END payload encoding & decoding */
int cn_encode_range_end(const struct cn_range_end *end,
                        uint8_t output[CN_RANGE_END_PAYLOAD_SIZE]);
int cn_decode_range_end(const uint8_t *payload, size_t length,
                        struct cn_range_end *end);

#endif
