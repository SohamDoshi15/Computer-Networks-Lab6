#ifndef CN_PROTOCOL_H
#define CN_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define CN_PROTOCOL_VERSION 1u
#define CN_FRAME_HEADER_SIZE 28u
#define CN_TRANSFER_ID_SIZE 16u
#define CN_SHA256_SIZE 32u
#define CN_MAX_PAYLOAD_SIZE (64u * 1024u * 1024u)
#define CN_MAX_TOPIC_SIZE 255u
#define CN_MAX_FILENAME_SIZE 4096u
#define CN_MAX_ERROR_TEXT_SIZE 4096u

enum cn_message_type {
    CN_MSG_GET = 1,
    CN_MSG_MANIFEST = 2,
    CN_MSG_FILE_START = 3,
    CN_MSG_DATA = 4,
    CN_MSG_FILE_END = 5,
    CN_MSG_ACK = 6,
    CN_MSG_ERROR = 7,
    CN_MSG_COMPLETE = 8,
    CN_MSG_CANCEL = 9,
    CN_MSG_RESUME = 10,
    CN_MSG_RESUME_OK = 11,
    CN_MSG_ASSIGN_RANGE = 12,
    CN_MSG_RANGE_READY = 13,
    CN_MSG_ACK_RANGE = 14,
    CN_MSG_RANGE_END = 15
};

enum cn_error_code {
    CN_ERR_INVALID_REQUEST = 1,
    CN_ERR_UNKNOWN_TOPIC = 2,
    CN_ERR_UNSAFE_PATH = 3,
    CN_ERR_FILE_IO = 4,
    CN_ERR_PROTOCOL = 5,
    CN_ERR_RESUME_REJECTED = 6,
    CN_ERR_RANGE_REJECTED = 7,
    CN_ERR_INTERNAL = 8
};

struct cn_frame_header {
    uint8_t version;
    uint8_t message_type;
    uint16_t flags;
    uint64_t payload_length;
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
};

enum cn_protocol_result {
    CN_PROTOCOL_OK = 0,
    CN_PROTOCOL_EOF = 1,
    CN_PROTOCOL_TRUNCATED = 2,
    CN_PROTOCOL_IO_ERROR = -1,
    CN_PROTOCOL_INVALID = -2,
    CN_PROTOCOL_TOO_LARGE = -3,
    CN_PROTOCOL_NO_MEMORY = -4
};

int cn_message_type_is_valid(uint8_t message_type);
int cn_header_is_valid(const struct cn_frame_header *header);
int cn_encode_header(const struct cn_frame_header *header,
                     uint8_t output[CN_FRAME_HEADER_SIZE]);
int cn_decode_header(const uint8_t input[CN_FRAME_HEADER_SIZE],
                     struct cn_frame_header *header);

uint64_t cn_host_to_network_u64(uint64_t value);
uint64_t cn_network_to_host_u64(uint64_t value);

#endif
