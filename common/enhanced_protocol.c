#include "enhanced_protocol.h"
#include "serialization.h"
#include <string.h>

int cn_encode_assign_range(const struct cn_range_assignment *range,
                           uint8_t output[CN_ASSIGN_RANGE_PAYLOAD_SIZE])
{
    struct cn_writer writer;
    if (range == NULL || output == NULL) {
        return -1;
    }
    cn_writer_init(&writer, output, CN_ASSIGN_RANGE_PAYLOAD_SIZE);
    if (cn_writer_put_u32(&writer, range->range_id) != 0 ||
        cn_writer_put_u32(&writer, range->file_index) != 0 ||
        cn_writer_put_u64(&writer, range->start_offset) != 0 ||
        cn_writer_put_u64(&writer, range->end_offset) != 0) {
        return -1;
    }
    return 0;
}

int cn_decode_assign_range(const uint8_t *payload, size_t length,
                           struct cn_range_assignment *range)
{
    struct cn_reader reader;
    if (payload == NULL || length != CN_ASSIGN_RANGE_PAYLOAD_SIZE || range == NULL) {
        return -1;
    }
    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &range->range_id) != 0 ||
        cn_reader_get_u32(&reader, &range->file_index) != 0 ||
        cn_reader_get_u64(&reader, &range->start_offset) != 0 ||
        cn_reader_get_u64(&reader, &range->end_offset) != 0) {
        return -1;
    }
    return 0;
}

int cn_encode_range_ready(const struct cn_range_assignment *range,
                          uint8_t output[CN_RANGE_READY_PAYLOAD_SIZE])
{
    return cn_encode_assign_range(range, output);
}

int cn_decode_range_ready(const uint8_t *payload, size_t length,
                          struct cn_range_assignment *range)
{
    return cn_decode_assign_range(payload, length, range);
}

int cn_encode_ack_range(const struct cn_range_ack *ack,
                        uint8_t output[CN_ACK_RANGE_PAYLOAD_SIZE])
{
    struct cn_writer writer;
    if (ack == NULL || output == NULL) {
        return -1;
    }
    cn_writer_init(&writer, output, CN_ACK_RANGE_PAYLOAD_SIZE);
    if (cn_writer_put_u32(&writer, ack->range_id) != 0 ||
        cn_writer_put_u32(&writer, ack->file_index) != 0 ||
        cn_writer_put_u64(&writer, ack->contiguous_offset) != 0) {
        return -1;
    }
    return 0;
}

int cn_decode_ack_range(const uint8_t *payload, size_t length,
                        struct cn_range_ack *ack)
{
    struct cn_reader reader;
    if (payload == NULL || length != CN_ACK_RANGE_PAYLOAD_SIZE || ack == NULL) {
        return -1;
    }
    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &ack->range_id) != 0 ||
        cn_reader_get_u32(&reader, &ack->file_index) != 0 ||
        cn_reader_get_u64(&reader, &ack->contiguous_offset) != 0) {
        return -1;
    }
    return 0;
}

int cn_encode_range_end(const struct cn_range_end *end,
                        uint8_t output[CN_RANGE_END_PAYLOAD_SIZE])
{
    struct cn_writer writer;
    if (end == NULL || output == NULL) {
        return -1;
    }
    cn_writer_init(&writer, output, CN_RANGE_END_PAYLOAD_SIZE);
    if (cn_writer_put_u32(&writer, end->range_id) != 0 ||
        cn_writer_put_u32(&writer, end->file_index) != 0 ||
        cn_writer_put_u64(&writer, end->end_offset) != 0) {
        return -1;
    }
    return 0;
}

int cn_decode_range_end(const uint8_t *payload, size_t length,
                        struct cn_range_end *end)
{
    struct cn_reader reader;
    if (payload == NULL || length != CN_RANGE_END_PAYLOAD_SIZE || end == NULL) {
        return -1;
    }
    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, &end->range_id) != 0 ||
        cn_reader_get_u32(&reader, &end->file_index) != 0 ||
        cn_reader_get_u64(&reader, &end->end_offset) != 0) {
        return -1;
    }
    return 0;
}
