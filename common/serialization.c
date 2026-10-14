#include "serialization.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <limits.h>
#include <string.h>

static int writer_reserve(struct cn_writer *writer, size_t length)
{
    if (writer == NULL || writer->failed || writer->offset > writer->capacity ||
        length > writer->capacity - writer->offset) {
        if (writer != NULL) {
            writer->failed = 1;
        }
        return -1;
    }
    return 0;
}

static int reader_reserve(struct cn_reader *reader, size_t length)
{
    if (reader == NULL || reader->failed || reader->offset > reader->length ||
        length > reader->length - reader->offset) {
        if (reader != NULL) {
            reader->failed = 1;
        }
        return -1;
    }
    return 0;
}

void cn_writer_init(struct cn_writer *writer, uint8_t *buffer, size_t capacity)
{
    if (writer == NULL) {
        return;
    }
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->offset = 0;
    writer->failed = buffer == NULL && capacity != 0;
}

int cn_writer_put_bytes(struct cn_writer *writer, const void *data, size_t length)
{
    if ((data == NULL && length != 0) || writer_reserve(writer, length) != 0) {
        return -1;
    }
    if (length != 0) {
        memcpy(writer->buffer + writer->offset, data, length);
        writer->offset += length;
    }
    return 0;
}

int cn_writer_put_u8(struct cn_writer *writer, uint8_t value)
{
    return cn_writer_put_bytes(writer, &value, sizeof(value));
}

int cn_writer_put_u16(struct cn_writer *writer, uint16_t value)
{
    const uint16_t network_value = htons(value);
    return cn_writer_put_bytes(writer, &network_value, sizeof(network_value));
}

int cn_writer_put_u32(struct cn_writer *writer, uint32_t value)
{
    const uint32_t network_value = htonl(value);
    return cn_writer_put_bytes(writer, &network_value, sizeof(network_value));
}

int cn_writer_put_u64(struct cn_writer *writer, uint64_t value)
{
    const uint64_t network_value = cn_host_to_network_u64(value);
    return cn_writer_put_bytes(writer, &network_value, sizeof(network_value));
}

int cn_writer_put_string(struct cn_writer *writer, const char *value,
                         size_t max_length)
{
    size_t length;

    if (writer == NULL || value == NULL) {
        return -1;
    }
    length = strlen(value);
    if (length > max_length || length > UINT32_MAX) {
        writer->failed = 1;
        return -1;
    }
    if (cn_writer_put_u32(writer, (uint32_t)length) != 0) {
        return -1;
    }
    return cn_writer_put_bytes(writer, value, length);
}

void cn_reader_init(struct cn_reader *reader, const uint8_t *buffer, size_t length)
{
    if (reader == NULL) {
        return;
    }
    reader->buffer = buffer;
    reader->length = length;
    reader->offset = 0;
    reader->failed = buffer == NULL && length != 0;
}

int cn_reader_get_bytes(struct cn_reader *reader, void *output, size_t length)
{
    if ((output == NULL && length != 0) || reader_reserve(reader, length) != 0) {
        return -1;
    }
    if (length != 0) {
        memcpy(output, reader->buffer + reader->offset, length);
        reader->offset += length;
    }
    return 0;
}

int cn_reader_get_u8(struct cn_reader *reader, uint8_t *value)
{
    return cn_reader_get_bytes(reader, value, sizeof(*value));
}

int cn_reader_get_u16(struct cn_reader *reader, uint16_t *value)
{
    uint16_t network_value;
    if (value == NULL || cn_reader_get_bytes(reader, &network_value, sizeof(network_value)) != 0) {
        return -1;
    }
    *value = ntohs(network_value);
    return 0;
}

int cn_reader_get_u32(struct cn_reader *reader, uint32_t *value)
{
    uint32_t network_value;
    if (value == NULL || cn_reader_get_bytes(reader, &network_value, sizeof(network_value)) != 0) {
        return -1;
    }
    *value = ntohl(network_value);
    return 0;
}

int cn_reader_get_u64(struct cn_reader *reader, uint64_t *value)
{
    uint64_t network_value;
    if (value == NULL || cn_reader_get_bytes(reader, &network_value, sizeof(network_value)) != 0) {
        return -1;
    }
    *value = cn_network_to_host_u64(network_value);
    return 0;
}

int cn_reader_get_string(struct cn_reader *reader, char *output,
                         size_t output_capacity, size_t max_length)
{
    uint32_t length;

    if (output == NULL || output_capacity == 0 ||
        cn_reader_get_u32(reader, &length) != 0 ||
        length > max_length || (size_t)length >= output_capacity) {
        if (reader != NULL) {
            reader->failed = 1;
        }
        return -1;
    }
    if (cn_reader_get_bytes(reader, output, length) != 0) {
        return -1;
    }
    output[length] = '\0';
    return 0;
}

size_t cn_reader_remaining(const struct cn_reader *reader)
{
    if (reader == NULL || reader->failed || reader->offset > reader->length) {
        return 0;
    }
    return reader->length - reader->offset;
}
