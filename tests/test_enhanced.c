#define _POSIX_C_SOURCE 200809L

#include "../common/enhanced_protocol.h"
#include "../common/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_assign_range_codec(void)
{
    struct cn_range_assignment orig, decoded;
    uint8_t buffer[CN_ASSIGN_RANGE_PAYLOAD_SIZE];

    orig.range_id = 101;
    orig.file_index = 5;
    orig.start_offset = 1048576ULL;
    orig.end_offset = 2097152ULL;

    assert(cn_encode_assign_range(&orig, buffer) == 0);
    assert(cn_decode_assign_range(buffer, sizeof(buffer), &decoded) == 0);

    assert(decoded.range_id == orig.range_id);
    assert(decoded.file_index == orig.file_index);
    assert(decoded.start_offset == orig.start_offset);
    assert(decoded.end_offset == orig.end_offset);

    /* Malformed / short payload check */
    assert(cn_decode_assign_range(buffer, sizeof(buffer) - 1, &decoded) != 0);
    assert(cn_decode_assign_range(NULL, sizeof(buffer), &decoded) != 0);
}

static void test_ack_range_codec(void)
{
    struct cn_range_ack orig, decoded;
    uint8_t buffer[CN_ACK_RANGE_PAYLOAD_SIZE];

    orig.range_id = 202;
    orig.file_index = 7;
    orig.contiguous_offset = 524288ULL;

    assert(cn_encode_ack_range(&orig, buffer) == 0);
    assert(cn_decode_ack_range(buffer, sizeof(buffer), &decoded) == 0);

    assert(decoded.range_id == orig.range_id);
    assert(decoded.file_index == orig.file_index);
    assert(decoded.contiguous_offset == orig.contiguous_offset);
}

static void test_range_end_codec(void)
{
    struct cn_range_end orig, decoded;
    uint8_t buffer[CN_RANGE_END_PAYLOAD_SIZE];

    orig.range_id = 303;
    orig.file_index = 9;
    orig.end_offset = 4194304ULL;

    assert(cn_encode_range_end(&orig, buffer) == 0);
    assert(cn_decode_range_end(buffer, sizeof(buffer), &decoded) == 0);

    assert(decoded.range_id == orig.range_id);
    assert(decoded.file_index == orig.file_index);
    assert(decoded.end_offset == orig.end_offset);
}

int main(void)
{
    test_assign_range_codec();
    test_ack_range_codec();
    test_range_end_codec();
    printf("test_enhanced: all protocol tests passed\n");
    return 0;
}
