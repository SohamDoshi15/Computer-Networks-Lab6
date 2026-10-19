#ifndef CN_CHECKPOINT_H
#define CN_CHECKPOINT_H

#include "../common/case2_protocol.h"
#include "../common/protocol.h"

#include <stdint.h>

#define CN_CHECKPOINT_MAGIC 0x434e4350u /* 'CNCP' */
#define CN_CHECKPOINT_VERSION 1u

struct cn_checkpoint {
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint32_t file_index;
    uint64_t contiguous_offset;
    uint32_t completed_count;
    struct cn_completed_file_entry *completed;
};

int cn_checkpoint_init(struct cn_checkpoint *ckpt,
                       const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                       const char *topic,
                       const uint8_t manifest_identity[CN_SHA256_SIZE]);

int cn_checkpoint_load(const char *output_dir, const char *topic,
                       struct cn_checkpoint *ckpt);

int cn_checkpoint_save(const char *output_dir, const char *topic,
                       const struct cn_checkpoint *ckpt);

int cn_checkpoint_add_completed(struct cn_checkpoint *ckpt,
                                uint32_t file_index,
                                const uint8_t sha256[CN_SHA256_SIZE]);

void cn_checkpoint_remove(const char *output_dir, const char *topic);

void cn_checkpoint_free(struct cn_checkpoint *ckpt);

int cn_checkpoint_truncate_part_file(const char *output_dir, const char *topic,
                                     const char *file_name, uint64_t target_offset);

#endif
