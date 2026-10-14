#include "protocol.h"

#include <arpa/inet.h>
#include <string.h>

uint64_t cn_host_to_network_u64(uint64_t value)
{
    const uint16_t endian_probe = 1;
    const uint32_t high = (uint32_t)(value >> 32);
    const uint32_t low = (uint32_t)(value & UINT32_C(0xffffffff));

    if (*(const uint8_t *)&endian_probe == 0) {
        return value;
    }
    return ((uint64_t)htonl(low) << 32) | htonl(high);
}

uint64_t cn_network_to_host_u64(uint64_t value)
{
    const uint16_t endian_probe = 1;
    const uint32_t high = (uint32_t)(value >> 32);
    const uint32_t low = (uint32_t)(value & UINT32_C(0xffffffff));

    if (*(const uint8_t *)&endian_probe == 0) {
        return value;
    }
    return ((uint64_t)ntohl(low) << 32) | ntohl(high);
}

int cn_message_type_is_valid(uint8_t message_type)
{
    return message_type >= CN_MSG_GET && message_type <= CN_MSG_RANGE_END;
}

int cn_header_is_valid(const struct cn_frame_header *header)
{
    if (header == NULL || header->version != CN_PROTOCOL_VERSION) {
        return 0;
    }
    if (!cn_message_type_is_valid(header->message_type)) {
        return 0;
    }
    return header->payload_length <= CN_MAX_PAYLOAD_SIZE;
}

int cn_encode_header(const struct cn_frame_header *header,
                     uint8_t output[CN_FRAME_HEADER_SIZE])
{
    uint16_t network_flags;
    uint64_t network_payload_length;

    if (output == NULL || !cn_header_is_valid(header)) {
        return CN_PROTOCOL_INVALID;
    }

    output[0] = header->version;
    output[1] = header->message_type;
    network_flags = htons(header->flags);
    network_payload_length = cn_host_to_network_u64(header->payload_length);
    memcpy(output + 2, &network_flags, sizeof(network_flags));
    memcpy(output + 4, &network_payload_length, sizeof(network_payload_length));
    memcpy(output + 12, header->transfer_id, CN_TRANSFER_ID_SIZE);
    return CN_PROTOCOL_OK;
}

int cn_decode_header(const uint8_t input[CN_FRAME_HEADER_SIZE],
                     struct cn_frame_header *header)
{
    uint16_t network_flags;
    uint64_t network_payload_length;

    if (input == NULL || header == NULL) {
        return CN_PROTOCOL_INVALID;
    }

    header->version = input[0];
    header->message_type = input[1];
    memcpy(&network_flags, input + 2, sizeof(network_flags));
    memcpy(&network_payload_length, input + 4, sizeof(network_payload_length));
    header->flags = ntohs(network_flags);
    header->payload_length = cn_network_to_host_u64(network_payload_length);
    memcpy(header->transfer_id, input + 12, CN_TRANSFER_ID_SIZE);

    if (header->payload_length > CN_MAX_PAYLOAD_SIZE) {
        return CN_PROTOCOL_TOO_LARGE;
    }
    return cn_header_is_valid(header) ? CN_PROTOCOL_OK : CN_PROTOCOL_INVALID;
}
