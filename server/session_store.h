#ifndef CN_SESSION_STORE_H
#define CN_SESSION_STORE_H

#include "../common/protocol.h"
#include "server_common.h"

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
    struct part1_manifest files; /* optional; see cn_session_store_attach_files() */
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

int cn_session_store_get_topic(struct cn_session_store *store,
                               const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                               char topic_out[CN_MAX_TOPIC_SIZE + 1]);

/*
 * Moves the topic's file list into the session, so later requests on other
 * connections can look files up without rescanning and rehashing the topic.
 * The session frees it on removal or expiry; *manifest is left empty.
 */
int cn_session_store_attach_files(struct cn_session_store *store,
                                  const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                  struct part1_manifest *manifest);

/*
 * Copies the topic and one attached file's name and size out of a session.
 * Returns 0, -1 when there is no live session, -2 when file_index is out of
 * range, or -3 when out of memory. The caller frees *name_out.
 */
int cn_session_store_get_file(struct cn_session_store *store,
                              const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                              uint32_t file_index,
                              char topic_out[CN_MAX_TOPIC_SIZE + 1],
                              char **name_out, uint64_t *size_out);

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
