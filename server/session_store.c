#define _POSIX_C_SOURCE 200809L

#include "session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_session(struct cn_session *session)
{
    part1_free_manifest(&session->files);
    free(session);
}

int cn_session_store_init(struct cn_session_store *store, uint32_t timeout_seconds)
{
    if (store == NULL) return -1;
    if (pthread_mutex_init(&store->lock, NULL) != 0) return -1;
    store->head = NULL;
    store->timeout_seconds = timeout_seconds > 0 ? timeout_seconds : CN_DEFAULT_SESSION_TIMEOUT_SEC;
    return 0;
}

void cn_session_store_destroy(struct cn_session_store *store)
{
    struct cn_session *curr, *next;
    if (store == NULL) return;
    pthread_mutex_lock(&store->lock);
    curr = store->head;
    while (curr != NULL) {
        next = curr->next;
        free_session(curr);
        curr = next;
    }
    store->head = NULL;
    pthread_mutex_unlock(&store->lock);
    pthread_mutex_destroy(&store->lock);
}

void cn_session_store_clean_expired(struct cn_session_store *store)
{
    time_t now = time(NULL);
    struct cn_session **curr_ptr;
    if (store == NULL) return;

    curr_ptr = &store->head;
    while (*curr_ptr != NULL) {
        struct cn_session *entry = *curr_ptr;
        if (difftime(now, entry->last_active) > (double)store->timeout_seconds) {
            *curr_ptr = entry->next;
            free_session(entry);
        } else {
            curr_ptr = &entry->next;
        }
    }
}

int cn_session_store_create(struct cn_session_store *store,
                            const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                            const char *topic,
                            const uint8_t manifest_identity[CN_SHA256_SIZE])
{
    struct cn_session *curr;
    if (store == NULL || transfer_id == NULL || topic == NULL || manifest_identity == NULL) {
        return -1;
    }

    pthread_mutex_lock(&store->lock);
    cn_session_store_clean_expired(store);

    curr = store->head;
    while (curr != NULL) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            break;
        }
        curr = curr->next;
    }

    if (curr == NULL) {
        curr = calloc(1, sizeof(*curr));
        if (curr == NULL) {
            pthread_mutex_unlock(&store->lock);
            return -1;
        }
        memcpy(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
        curr->next = store->head;
        store->head = curr;
    } else {
        part1_free_manifest(&curr->files);
    }

    (void)snprintf(curr->topic, sizeof(curr->topic), "%s", topic);
    memcpy(curr->manifest_identity, manifest_identity, CN_SHA256_SIZE);
    curr->last_file_index = 0;
    curr->last_offset = 0;
    curr->last_active = time(NULL);
    curr->completed = 0;

    pthread_mutex_unlock(&store->lock);
    return 0;
}

int cn_session_store_update_ack(struct cn_session_store *store,
                                const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                uint32_t file_index,
                                uint64_t contiguous_offset)
{
    struct cn_session *curr;
    int found = 0;

    if (store == NULL || transfer_id == NULL) return -1;

    pthread_mutex_lock(&store->lock);
    curr = store->head;
    while (curr != NULL) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            found = 1;
            /* Advance only forward */
            if ((file_index > curr->last_file_index) ||
                (file_index == curr->last_file_index && contiguous_offset > curr->last_offset)) {
                curr->last_file_index = file_index;
                curr->last_offset = contiguous_offset;
            }
            curr->last_active = time(NULL);
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&store->lock);
    return found ? 0 : -1;
}

int cn_session_store_get_topic(struct cn_session_store *store,
                               const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                               char topic_out[CN_MAX_TOPIC_SIZE + 1])
{
    struct cn_session *curr;
    int found = 0;

    if (store == NULL || transfer_id == NULL || topic_out == NULL) return -1;

    pthread_mutex_lock(&store->lock);
    cn_session_store_clean_expired(store);
    curr = store->head;
    while (curr != NULL) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            (void)snprintf(topic_out, CN_MAX_TOPIC_SIZE + 1, "%s", curr->topic);
            curr->last_active = time(NULL);
            found = 1;
            break;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&store->lock);
    return found ? 0 : -1;
}

int cn_session_store_attach_files(struct cn_session_store *store,
                                  const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                  struct part1_manifest *manifest)
{
    struct cn_session *curr;
    int found = 0;

    if (store == NULL || transfer_id == NULL || manifest == NULL) return -1;

    pthread_mutex_lock(&store->lock);
    for (curr = store->head; curr != NULL; curr = curr->next) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            part1_free_manifest(&curr->files);
            curr->files = *manifest;
            memset(manifest, 0, sizeof(*manifest));
            curr->last_active = time(NULL);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&store->lock);
    return found ? 0 : -1;
}

int cn_session_store_get_file(struct cn_session_store *store,
                              const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                              uint32_t file_index,
                              char topic_out[CN_MAX_TOPIC_SIZE + 1],
                              char **name_out, uint64_t *size_out)
{
    struct cn_session *curr;
    int result = -1;

    if (store == NULL || transfer_id == NULL || topic_out == NULL || name_out == NULL ||
        size_out == NULL) {
        return -1;
    }
    *name_out = NULL;

    pthread_mutex_lock(&store->lock);
    cn_session_store_clean_expired(store);
    for (curr = store->head; curr != NULL; curr = curr->next) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) != 0) {
            continue;
        }
        curr->last_active = time(NULL);
        if (file_index >= curr->files.count) {
            result = -2;
            break;
        }
        *name_out = strdup(curr->files.files[file_index].name);
        if (*name_out == NULL) {
            result = -3;
            break;
        }
        *size_out = curr->files.files[file_index].size;
        (void)snprintf(topic_out, CN_MAX_TOPIC_SIZE + 1, "%s", curr->topic);
        result = 0;
        break;
    }
    pthread_mutex_unlock(&store->lock);
    return result;
}

void cn_session_store_remove(struct cn_session_store *store,
                             const uint8_t transfer_id[CN_TRANSFER_ID_SIZE])
{
    struct cn_session **curr_ptr;
    if (store == NULL || transfer_id == NULL) return;

    pthread_mutex_lock(&store->lock);
    curr_ptr = &store->head;
    while (*curr_ptr != NULL) {
        struct cn_session *entry = *curr_ptr;
        if (memcmp(entry->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            *curr_ptr = entry->next;
            free_session(entry);
            break;
        }
        curr_ptr = &entry->next;
    }
    pthread_mutex_unlock(&store->lock);
}

int cn_session_store_validate_resume(struct cn_session_store *store,
                                     const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                                     const char *topic,
                                     const uint8_t manifest_identity[CN_SHA256_SIZE],
                                     uint32_t req_file_index,
                                     uint64_t req_offset,
                                     uint32_t *accepted_file_index,
                                     uint64_t *accepted_offset,
                                     char *out_reason, size_t out_reason_cap)
{
    struct cn_session *curr;
    time_t now;

    if (store == NULL || transfer_id == NULL || topic == NULL || manifest_identity == NULL ||
        accepted_file_index == NULL || accepted_offset == NULL) {
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "internal error in session store validation");
        }
        return -1;
    }

    pthread_mutex_lock(&store->lock);
    cn_session_store_clean_expired(store);

    curr = store->head;
    while (curr != NULL) {
        if (memcmp(curr->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE) == 0) {
            break;
        }
        curr = curr->next;
    }

    if (curr == NULL) {
        pthread_mutex_unlock(&store->lock);
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "session not found or expired on server");
        }
        return -1;
    }

    now = time(NULL);
    if (difftime(now, curr->last_active) > (double)store->timeout_seconds) {
        /* Session expired */
        pthread_mutex_unlock(&store->lock);
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "session has expired");
        }
        return -1;
    }

    if (curr->completed) {
        pthread_mutex_unlock(&store->lock);
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "transfer was already completed");
        }
        return -1;
    }

    if (strcmp(curr->topic, topic) != 0) {
        pthread_mutex_unlock(&store->lock);
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "requested topic '%s' does not match session topic '%s'",
                           topic, curr->topic);
        }
        return -1;
    }

    if (memcmp(curr->manifest_identity, manifest_identity, CN_SHA256_SIZE) != 0) {
        pthread_mutex_unlock(&store->lock);
        if (out_reason && out_reason_cap > 0) {
            (void)snprintf(out_reason, out_reason_cap, "manifest identity mismatch");
        }
        return -1;
    }

    /* Resume no further than both sides have persisted. The client writes its
       checkpoint before sending the ACK, so after a cut it is usually ahead of
       the progress stored here; accept the earlier point instead of rejecting. */
    if ((req_file_index > curr->last_file_index) ||
        (req_file_index == curr->last_file_index && req_offset > curr->last_offset)) {
        *accepted_file_index = curr->last_file_index;
        *accepted_offset = curr->last_offset;
    } else {
        *accepted_file_index = req_file_index;
        *accepted_offset = req_offset;
    }
    curr->last_active = now;

    pthread_mutex_unlock(&store->lock);
    return 0;
}
