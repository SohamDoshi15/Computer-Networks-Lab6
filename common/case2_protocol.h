#ifndef CN_CASE2_PROTOCOL_H
#define CN_CASE2_PROTOCOL_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

struct cn_completed_file_entry {
    uint32_t file_index;
    uint8_t sha256[CN_SHA256_SIZE];
};

struct cn_resume_request {
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint32_t file_index;
    uint64_t contiguous_offset;
    uint32_t completed_count;
    struct cn_completed_file_entry *completed;
};

int cn_compute_manifest_identity(const uint8_t *manifest_payload, size_t length,
                                uint8_t identity_out[CN_SHA256_SIZE]);

int cn_encode_resume(const struct cn_resume_request *req,
                     uint8_t **payload_out, size_t *size_out);

int cn_decode_resume(const uint8_t *payload, size_t length,
                     struct cn_resume_request *req);

void cn_resume_request_free(struct cn_resume_request *req);

int cn_encode_resume_ok(uint32_t accepted_file_index, uint64_t accepted_offset,
                        const uint8_t *manifest_payload, size_t manifest_length,
                        uint8_t **payload_out, size_t *size_out);

int cn_decode_resume_ok(const uint8_t *payload, size_t length,
                        uint32_t *accepted_file_index, uint64_t *accepted_offset,
                        const uint8_t **manifest_payload, size_t *manifest_length);

#endif
