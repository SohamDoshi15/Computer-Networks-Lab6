#ifndef CN_SESSION_STORE_H
#define CN_SESSION_STORE_H

#include "../common/protocol.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

#define CN_DEFAULT_SESSION_TIMEOUT_SEC 120u

struct cn_session {
    uint8_t transfer_id[CN_TRANSFER_ID_SIZE];
    char topic[CN_MAX_TOPIC_SIZE + 1];
    uint8_t manifest_identity[CN_SHA256_SIZE];
    uint32_t last_file_index;
    uint64_t last_offset;
    time_t last_active;
    int completed;
    struct cn_session *next;
};

struct cn_session_store {
    pthread_mutex_t lock;
    struct cn_session *head;
    uint32_t timeout_seconds;
};

int cn_session_store_init(struct cn_session_store *store, uint32_t timeout_seconds);
void cn_session_store_destroy(struct cn_session_store *store);

int cn_session_store_create(struct cn_session_store *store,
                            const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                            const char *topic,
                            const uint8_t manifest_identity[CN_SHA256_SIZE]);

int cn_session_store_update_ack(struct cn_session_store *store,
                                const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                uint32_t file_index,
                                uint64_t contiguous_offset);

void cn_session_store_remove(struct cn_session_store *store,
                             const uint8_t transfer_id[CN_TRANSFER_ID_SIZE]);

void cn_session_store_clean_expired(struct cn_session_store *store);

int cn_session_store_validate_resume(struct cn_session_store *store,
                                     const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                     const char *topic,
                                     const uint8_t manifest_identity[CN_SHA256_SIZE],
                                     uint32_t req_file_index,
                                     uint64_t req_offset,
                                     uint32_t *accepted_file_index,
                                     uint64_t *accepted_offset,
                                     char *out_reason, size_t out_reason_cap);

#endif
