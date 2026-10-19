#define _POSIX_C_SOURCE 200809L

#include "case2_protocol.h"
#include "serialization.h"

#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>

int cn_compute_manifest_identity(const uint8_t *manifest_payload, size_t length,
                                uint8_t identity_out[CN_SHA256_SIZE])
{
    unsigned int md_len = 0;
    if (manifest_payload == NULL && length > 0) return -1;
    if (identity_out == NULL) return -1;
    if (EVP_Digest(manifest_payload, length, identity_out, &md_len, EVP_sha256(), NULL) != 1 ||
        md_len != CN_SHA256_SIZE) {
        return -1;
    }
    return 0;
}

int cn_encode_resume(const struct cn_resume_request *req,
                     uint8_t **payload_out, size_t *size_out)
{
    size_t topic_len, capacity;
    uint8_t *payload;
    struct cn_writer writer;
    uint32_t i;

    if (req == NULL || payload_out == NULL || size_out == NULL) return -1;
    topic_len = strlen(req->topic);
    if (topic_len > CN_MAX_TOPIC_SIZE) return -1;

    /* 16 (transfer_id) + 4+topic_len + 32 (manifest_identity) + 4 (file_index) +
       8 (offset) + 4 (completed_count) + completed_count * (4 + 32) */
    capacity = 16 + 4 + topic_len + 32 + 4 + 8 + 4;
    if (req->completed_count > (SIZE_MAX - capacity) / 36) return -1;
    capacity += (size_t)req->completed_count * 36;
    if (capacity > CN_MAX_PAYLOAD_SIZE) return -1;

    payload = malloc(capacity);
    if (payload == NULL) return -1;

    cn_writer_init(&writer, payload, capacity);
    if (cn_writer_put_bytes(&writer, req->transfer_id, CN_TRANSFER_ID_SIZE) != 0 ||
        cn_writer_put_string(&writer, req->topic, CN_MAX_TOPIC_SIZE) != 0 ||
        cn_writer_put_bytes(&writer, req->manifest_identity, CN_SHA256_SIZE) != 0 ||
        cn_writer_put_u32(&writer, req->file_index) != 0 ||
        cn_writer_put_u64(&writer, req->contiguous_offset) != 0 ||
        cn_writer_put_u32(&writer, req->completed_count) != 0) {
        free(payload);
        return -1;
    }

    for (i = 0; i < req->completed_count; ++i) {
        if (cn_writer_put_u32(&writer, req->completed[i].file_index) != 0 ||
            cn_writer_put_bytes(&writer, req->completed[i].sha256, CN_SHA256_SIZE) != 0) {
            free(payload);
            return -1;
        }
    }

    *payload_out = payload;
    *size_out = writer.offset;
    return 0;
}

int cn_decode_resume(const uint8_t *payload, size_t length,
                     struct cn_resume_request *req)
{
    struct cn_reader reader;
    uint32_t i;

    if (payload == NULL || req == NULL) return -1;
    memset(req, 0, sizeof(*req));

    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_bytes(&reader, req->transfer_id, CN_TRANSFER_ID_SIZE) != 0 ||
        cn_reader_get_string(&reader, req->topic, sizeof(req->topic), CN_MAX_TOPIC_SIZE) != 0 ||
        cn_reader_get_bytes(&reader, req->manifest_identity, CN_SHA256_SIZE) != 0 ||
        cn_reader_get_u32(&reader, &req->file_index) != 0 ||
        cn_reader_get_u64(&reader, &req->contiguous_offset) != 0 ||
        cn_reader_get_u32(&reader, &req->completed_count) != 0) {
        return -1;
    }

    if (req->completed_count > 0) {
        if (cn_reader_remaining(&reader) < (size_t)req->completed_count * 36) {
            return -1;
        }
        req->completed = calloc(req->completed_count, sizeof(*req->completed));
        if (req->completed == NULL) return -1;

        for (i = 0; i < req->completed_count; ++i) {
            if (cn_reader_get_u32(&reader, &req->completed[i].file_index) != 0 ||
                cn_reader_get_bytes(&reader, req->completed[i].sha256, CN_SHA256_SIZE) != 0) {
                free(req->completed);
                req->completed = NULL;
                return -1;
            }
        }
    }

    if (cn_reader_remaining(&reader) != 0) {
        free(req->completed);
        req->completed = NULL;
        return -1;
    }
    return 0;
}

void cn_resume_request_free(struct cn_resume_request *req)
{
    if (req != NULL) {
        free(req->completed);
        req->completed = NULL;
    }
}

int cn_encode_resume_ok(uint32_t accepted_file_index, uint64_t accepted_offset,
                        const uint8_t *manifest_payload, size_t manifest_length,
                        uint8_t **payload_out, size_t *size_out)
{
    size_t capacity;
    uint8_t *payload;
    struct cn_writer writer;

    if (payload_out == NULL || size_out == NULL) return -1;
    if (manifest_length > CN_MAX_PAYLOAD_SIZE) return -1;

    capacity = 4 + 8 + manifest_length;
    if (capacity > CN_MAX_PAYLOAD_SIZE) return -1;

    payload = malloc(capacity);
    if (payload == NULL) return -1;

    cn_writer_init(&writer, payload, capacity);
    if (cn_writer_put_u32(&writer, accepted_file_index) != 0 ||
        cn_writer_put_u64(&writer, accepted_offset) != 0 ||
        (manifest_length > 0 && cn_writer_put_bytes(&writer, manifest_payload, manifest_length) != 0)) {
        free(payload);
        return -1;
    }

    *payload_out = payload;
    *size_out = writer.offset;
    return 0;
}

int cn_decode_resume_ok(const uint8_t *payload, size_t length,
                        uint32_t *accepted_file_index, uint64_t *accepted_offset,
                        const uint8_t **manifest_payload, size_t *manifest_length)
{
    struct cn_reader reader;

    if (payload == NULL || accepted_file_index == NULL || accepted_offset == NULL ||
        manifest_payload == NULL || manifest_length == NULL) {
        return -1;
    }

    cn_reader_init(&reader, payload, length);
    if (cn_reader_get_u32(&reader, accepted_file_index) != 0 ||
        cn_reader_get_u64(&reader, accepted_offset) != 0) {
        return -1;
    }

    *manifest_length = cn_reader_remaining(&reader);
    *manifest_payload = payload + reader.offset;
    return 0;
}
