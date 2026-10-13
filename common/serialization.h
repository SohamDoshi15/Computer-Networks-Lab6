#ifndef CN_SERIALIZATION_H
#define CN_SERIALIZATION_H

#include <stddef.h>
#include <stdint.h>

struct cn_writer {
    uint8_t *buffer;
    size_t capacity;
    size_t offset;
    int failed;
};

struct cn_reader {
    const uint8_t *buffer;
    size_t length;
    size_t offset;
    int failed;
};

void cn_writer_init(struct cn_writer *writer, uint8_t *buffer, size_t capacity);
int cn_writer_put_u8(struct cn_writer *writer, uint8_t value);
int cn_writer_put_u16(struct cn_writer *writer, uint16_t value);
int cn_writer_put_u32(struct cn_writer *writer, uint32_t value);
int cn_writer_put_u64(struct cn_writer *writer, uint64_t value);
int cn_writer_put_bytes(struct cn_writer *writer, const void *data, size_t length);
int cn_writer_put_string(struct cn_writer *writer, const char *value,
                         size_t max_length);

void cn_reader_init(struct cn_reader *reader, const uint8_t *buffer, size_t length);
int cn_reader_get_u8(struct cn_reader *reader, uint8_t *value);
int cn_reader_get_u16(struct cn_reader *reader, uint16_t *value);
int cn_reader_get_u32(struct cn_reader *reader, uint32_t *value);
int cn_reader_get_u64(struct cn_reader *reader, uint64_t *value);
int cn_reader_get_bytes(struct cn_reader *reader, void *output, size_t length);
int cn_reader_get_string(struct cn_reader *reader, char *output,
                         size_t output_capacity, size_t max_length);
size_t cn_reader_remaining(const struct cn_reader *reader);

#endif
