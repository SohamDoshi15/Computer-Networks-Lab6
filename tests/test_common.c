#include "cli.h"
#include "protocol.h"
#include "serialization.h"
#include "socket_io.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct fragmented_sender_args {
    int socket_fd;
    const uint8_t *data;
    size_t length;
};

static void *send_one_byte_at_a_time(void *opaque)
{
    struct fragmented_sender_args *args = opaque;
    size_t index;

    for (index = 0; index < args->length; ++index) {
        assert(send(args->socket_fd, args->data + index, 1, 0) == 1);
    }
    assert(shutdown(args->socket_fd, SHUT_WR) == 0);
    return NULL;
}

static void test_header_known_vector(void)
{
    struct cn_frame_header input = {0};
    struct cn_frame_header output = {0};
    uint8_t encoded[CN_FRAME_HEADER_SIZE];
    size_t index;

    input.version = CN_PROTOCOL_VERSION;
    input.message_type = CN_MSG_DATA;
    input.flags = UINT16_C(0x1234);
    input.payload_length = UINT64_C(0x0000000001020304);
    for (index = 0; index < CN_TRANSFER_ID_SIZE; ++index) {
        input.transfer_id[index] = (uint8_t)index;
    }

    assert(cn_encode_header(&input, encoded) == CN_PROTOCOL_OK);
    assert(encoded[0] == 1);
    assert(encoded[1] == CN_MSG_DATA);
    assert(encoded[2] == 0x12 && encoded[3] == 0x34);
    assert(encoded[4] == 0x00 && encoded[5] == 0x00);
    assert(encoded[6] == 0x00 && encoded[7] == 0x00);
    assert(encoded[8] == 0x01 && encoded[9] == 0x02);
    assert(encoded[10] == 0x03 && encoded[11] == 0x04);
    assert(memcmp(encoded + 12, input.transfer_id, CN_TRANSFER_ID_SIZE) == 0);

    assert(cn_decode_header(encoded, &output) == CN_PROTOCOL_OK);
    assert(output.version == input.version);
    assert(output.message_type == input.message_type);
    assert(output.flags == input.flags);
    assert(output.payload_length == input.payload_length);
    assert(memcmp(output.transfer_id, input.transfer_id, CN_TRANSFER_ID_SIZE) == 0);
}

static void test_header_validation(void)
{
    struct cn_frame_header header = {0};
    uint8_t encoded[CN_FRAME_HEADER_SIZE] = {0};

    header.version = 99;
    header.message_type = CN_MSG_GET;
    assert(cn_encode_header(&header, encoded) == CN_PROTOCOL_INVALID);

    encoded[0] = CN_PROTOCOL_VERSION;
    encoded[1] = 255;
    assert(cn_decode_header(encoded, &header) == CN_PROTOCOL_INVALID);

    encoded[1] = CN_MSG_DATA;
    encoded[4] = 0x01;
    assert(cn_decode_header(encoded, &header) == CN_PROTOCOL_TOO_LARGE);
}

static void test_serialization_round_trip(void)
{
    uint8_t buffer[128];
    struct cn_writer writer;
    struct cn_reader reader;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    char topic[CN_MAX_TOPIC_SIZE + 1];

    cn_writer_init(&writer, buffer, sizeof(buffer));
    assert(cn_writer_put_u8(&writer, 0xab) == 0);
    assert(cn_writer_put_u16(&writer, 0x1234) == 0);
    assert(cn_writer_put_u32(&writer, UINT32_C(0x89abcdef)) == 0);
    assert(cn_writer_put_u64(&writer, UINT64_C(0x0102030405060708)) == 0);
    assert(cn_writer_put_string(&writer, "Cane", CN_MAX_TOPIC_SIZE) == 0);

    cn_reader_init(&reader, buffer, writer.offset);
    assert(cn_reader_get_u8(&reader, &u8) == 0 && u8 == 0xab);
    assert(cn_reader_get_u16(&reader, &u16) == 0 && u16 == 0x1234);
    assert(cn_reader_get_u32(&reader, &u32) == 0 && u32 == UINT32_C(0x89abcdef));
    assert(cn_reader_get_u64(&reader, &u64) == 0 && u64 == UINT64_C(0x0102030405060708));
    assert(cn_reader_get_string(&reader, topic, sizeof(topic), CN_MAX_TOPIC_SIZE) == 0);
    assert(strcmp(topic, "Cane") == 0);
    assert(cn_reader_remaining(&reader) == 0);
}

static void test_serialization_bounds(void)
{
    uint8_t buffer[4] = {0};
    struct cn_writer writer;
    struct cn_reader reader;
    uint64_t value;

    cn_writer_init(&writer, buffer, sizeof(buffer));
    assert(cn_writer_put_u64(&writer, 1) == -1);
    assert(writer.failed);

    cn_reader_init(&reader, buffer, sizeof(buffer));
    assert(cn_reader_get_u64(&reader, &value) == -1);
    assert(reader.failed);
    assert(cn_writer_put_string(NULL, "Cane", CN_MAX_TOPIC_SIZE) == -1);
}

static void test_fragmented_receive(void)
{
    int sockets[2];
    pthread_t sender;
    uint8_t expected[4096];
    uint8_t actual[sizeof(expected)];
    struct fragmented_sender_args args;
    size_t index;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    for (index = 0; index < sizeof(expected); ++index) {
        expected[index] = (uint8_t)(index & 0xffu);
    }
    args.socket_fd = sockets[0];
    args.data = expected;
    args.length = sizeof(expected);
    assert(pthread_create(&sender, NULL, send_one_byte_at_a_time, &args) == 0);
    assert(cn_recv_all(sockets[1], actual, sizeof(actual)) == CN_PROTOCOL_OK);
    assert(memcmp(actual, expected, sizeof(expected)) == 0);
    assert(cn_recv_all(sockets[1], actual, 1) == CN_PROTOCOL_EOF);
    assert(pthread_join(sender, NULL) == 0);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_frame_round_trip(void)
{
    int sockets[2];
    struct cn_frame_header sent = {0};
    struct cn_frame_header received = {0};
    const uint8_t payload[] = {0x00, 0x11, 0x00, 0xff, 0x42};
    uint8_t *received_payload = NULL;
    size_t index;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sent.version = CN_PROTOCOL_VERSION;
    sent.message_type = CN_MSG_DATA;
    sent.payload_length = sizeof(payload);
    for (index = 0; index < CN_TRANSFER_ID_SIZE; ++index) {
        sent.transfer_id[index] = (uint8_t)(0xf0u + index);
    }

    assert(cn_send_frame(sockets[0], &sent, payload) == CN_PROTOCOL_OK);
    assert(cn_recv_frame(sockets[1], &received, &received_payload) == CN_PROTOCOL_OK);
    assert(received.message_type == CN_MSG_DATA);
    assert(received.payload_length == sizeof(payload));
    assert(memcmp(received.transfer_id, sent.transfer_id, CN_TRANSFER_ID_SIZE) == 0);
    assert(memcmp(received_payload, payload, sizeof(payload)) == 0);

    free(received_payload);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_truncated_frame(void)
{
    int sockets[2];
    struct cn_frame_header header = {0};
    uint8_t encoded[CN_FRAME_HEADER_SIZE];
    uint8_t *payload = NULL;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    header.version = CN_PROTOCOL_VERSION;
    header.message_type = CN_MSG_GET;
    header.payload_length = 10;
    assert(cn_encode_header(&header, encoded) == CN_PROTOCOL_OK);
    assert(cn_send_all(sockets[0], encoded, sizeof(encoded)) == CN_PROTOCOL_OK);
    assert(cn_send_all(sockets[0], "abc", 3) == CN_PROTOCOL_OK);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    assert(cn_recv_frame(sockets[1], &header, &payload) == CN_PROTOCOL_TRUNCATED);
    assert(payload == NULL);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_cli_parsing(void)
{
    uint16_t port;
    uint32_t count;
    double probability;

    assert(cn_parse_port("8080", &port) == 0 && port == 8080);
    assert(cn_parse_port("0", &port) == -1);
    assert(cn_parse_port("65536", &port) == -1);
    assert(cn_parse_port(" 80", &port) == -1);
    assert(cn_parse_port("80x", &port) == -1);

    assert(cn_parse_probability("0", &probability) == 0 && probability == 0.0);
    assert(cn_parse_probability("0.25", &probability) == 0 && probability == 0.25);
    assert(cn_parse_probability("1", &probability) == 0 && probability == 1.0);
    assert(cn_parse_probability("1.01", &probability) == -1);
    assert(cn_parse_probability("nan", &probability) == -1);

    assert(cn_parse_positive_u32("4", &count) == 0 && count == 4);
    assert(cn_parse_positive_u32("0", &count) == -1);
    assert(cn_parse_positive_u32("4294967296", &count) == -1);
}

int main(void)
{
    test_header_known_vector();
    test_header_validation();
    test_serialization_round_trip();
    test_serialization_bounds();
    test_fragmented_receive();
    test_frame_round_trip();
    test_truncated_frame();
    test_cli_parsing();
    puts("all common protocol tests passed");
    return 0;
}
