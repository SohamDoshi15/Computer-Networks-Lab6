#define _POSIX_C_SOURCE 200809L

#include "../common/case2_protocol.h"
#include "../common/protocol.h"
#include "../common/serialization.h"
#include "../server/session_store.h"
#include "../client/checkpoint.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_case2_protocol_encoding(void)
{
    struct cn_resume_request req, decoded;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint32_t i;
    struct cn_completed_file_entry completed[3];

    memset(&req, 0, sizeof(req));
    for (i = 0; i < CN_TRANSFER_ID_SIZE; ++i) req.transfer_id[i] = (uint8_t)(i + 1);
    (void)snprintf(req.topic, sizeof(req.topic), "Scoiattolo");
    for (i = 0; i < CN_SHA256_SIZE; ++i) req.manifest_identity[i] = (uint8_t)(0xaa ^ i);
    req.file_index = 42;
    req.contiguous_offset = 123456789ULL;
    req.completed_count = 3;

    for (i = 0; i < 3; ++i) {
        completed[i].file_index = i;
        memset(completed[i].sha256, (int)(i + 10), CN_SHA256_SIZE);
    }
    req.completed = completed;

    assert(cn_encode_resume(&req, &payload, &payload_len) == 0);
    assert(payload != NULL && payload_len > 0);

    assert(cn_decode_resume(payload, payload_len, &decoded) == 0);
    assert(memcmp(req.transfer_id, decoded.transfer_id, CN_TRANSFER_ID_SIZE) == 0);
    assert(strcmp(req.topic, decoded.topic) == 0);
    assert(memcmp(req.manifest_identity, decoded.manifest_identity, CN_SHA256_SIZE) == 0);
    assert(req.file_index == decoded.file_index);
    assert(req.contiguous_offset == decoded.contiguous_offset);
    assert(req.completed_count == decoded.completed_count);

    for (i = 0; i < 3; ++i) {
        assert(decoded.completed[i].file_index == completed[i].file_index);
        assert(memcmp(decoded.completed[i].sha256, completed[i].sha256, CN_SHA256_SIZE) == 0);
    }
    cn_resume_request_free(&decoded);
    free(payload);

    /* Test RESUME_OK encode/decode */
    {
        uint8_t manifest_dummy[] = {0x01, 0x02, 0x03, 0x04, 0x05};
        uint8_t *ok_payload = NULL;
        size_t ok_len = 0;
        uint32_t acc_idx = 0;
        uint64_t acc_off = 0;
        const uint8_t *dec_manifest = NULL;
        size_t dec_manifest_len = 0;

        assert(cn_encode_resume_ok(15, 65536, manifest_dummy, sizeof(manifest_dummy),
                                   &ok_payload, &ok_len) == 0);
        assert(ok_payload != NULL && ok_len == 4 + 8 + sizeof(manifest_dummy));

        assert(cn_decode_resume_ok(ok_payload, ok_len, &acc_idx, &acc_off,
                                  &dec_manifest, &dec_manifest_len) == 0);
        assert(acc_idx == 15);
        assert(acc_off == 65536);
        assert(dec_manifest_len == sizeof(manifest_dummy));
        assert(memcmp(dec_manifest, manifest_dummy, sizeof(manifest_dummy)) == 0);
        free(ok_payload);
    }

    /* Test manifest identity calculation */
    {
        uint8_t data1[] = "manifest data 1";
        uint8_t data2[] = "manifest data 2";
        uint8_t hash1[CN_SHA256_SIZE], hash1_again[CN_SHA256_SIZE], hash2[CN_SHA256_SIZE];

        assert(cn_compute_manifest_identity(data1, sizeof(data1), hash1) == 0);
        assert(cn_compute_manifest_identity(data1, sizeof(data1), hash1_again) == 0);
        assert(cn_compute_manifest_identity(data2, sizeof(data2), hash2) == 0);
        assert(memcmp(hash1, hash1_again, CN_SHA256_SIZE) == 0);
        assert(memcmp(hash1, hash2, CN_SHA256_SIZE) != 0);
    }
}

static void test_session_store(void)
{
    struct cn_session_store store;
    uint8_t id1[CN_TRANSFER_ID_SIZE] = {1, 2, 3};
    uint8_t id2[CN_TRANSFER_ID_SIZE] = {4, 5, 6};
    uint8_t hash[CN_SHA256_SIZE] = {0x55};
    uint8_t wrong_hash[CN_SHA256_SIZE] = {0x99};
    uint32_t acc_idx;
    uint64_t acc_off;
    char reason[128];

    assert(cn_session_store_init(&store, 2) == 0);

    assert(cn_session_store_create(&store, id1, "Cane", hash) == 0);
    assert(cn_session_store_update_ack(&store, id1, 1, 1000) == 0);
    assert(cn_session_store_update_ack(&store, id1, 2, 500) == 0);

    /* Valid resume: exact match */
    assert(cn_session_store_validate_resume(&store, id1, "Cane", hash, 2, 500,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) == 0);
    assert(acc_idx == 2 && acc_off == 500);

    /* Valid resume: earlier checkpoint */
    assert(cn_session_store_validate_resume(&store, id1, "Cane", hash, 1, 200,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) == 0);
    assert(acc_idx == 1 && acc_off == 200);

    /* Invalid resume: requested offset exceeds acknowledged */
    assert(cn_session_store_validate_resume(&store, id1, "Cane", hash, 2, 600,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Invalid resume: requested file exceeds acknowledged */
    assert(cn_session_store_validate_resume(&store, id1, "Cane", hash, 3, 0,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Invalid resume: topic mismatch */
    assert(cn_session_store_validate_resume(&store, id1, "Gatto", hash, 2, 500,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Invalid resume: manifest identity mismatch */
    assert(cn_session_store_validate_resume(&store, id1, "Cane", wrong_hash, 2, 500,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Invalid resume: unknown transfer id */
    assert(cn_session_store_validate_resume(&store, id2, "Cane", hash, 0, 0,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Test removal */
    cn_session_store_remove(&store, id1);
    assert(cn_session_store_validate_resume(&store, id1, "Cane", hash, 0, 0,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    /* Test timeout and expiration */
    assert(cn_session_store_create(&store, id2, "Mucca", hash) == 0);
    assert(cn_session_store_validate_resume(&store, id2, "Mucca", hash, 0, 0,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) == 0);
    sleep(3); /* Wait for 2s timeout */
    assert(cn_session_store_validate_resume(&store, id2, "Mucca", hash, 0, 0,
                                           &acc_idx, &acc_off, reason, sizeof(reason)) != 0);

    cn_session_store_destroy(&store);
}

static void test_client_checkpoint(void)
{
    char tmp_dir[] = "/tmp/cn_test_ckpt_XXXXXX";
    char topic_dir[1024];
    struct cn_checkpoint ckpt, loaded;
    uint8_t id[CN_TRANSFER_ID_SIZE] = {0x11, 0x22, 0x33, 0x44};
    uint8_t hash[CN_SHA256_SIZE] = {0x77};
    uint8_t file_hash1[CN_SHA256_SIZE] = {0xaa};
    uint8_t file_hash2[CN_SHA256_SIZE] = {0xbb};
    char part_path[2048];
    struct stat st;
    int fd;

    assert(mkdtemp(tmp_dir) != NULL);
    (void)snprintf(topic_dir, sizeof(topic_dir), "%s/Elefante", tmp_dir);
    assert(mkdir(topic_dir, 0777) == 0);

    assert(cn_checkpoint_init(&ckpt, id, "Elefante", hash) == 0);
    ckpt.file_index = 5;
    ckpt.contiguous_offset = 65536;
    assert(cn_checkpoint_add_completed(&ckpt, 0, file_hash1) == 0);
    assert(cn_checkpoint_add_completed(&ckpt, 1, file_hash2) == 0);

    assert(cn_checkpoint_save(tmp_dir, "Elefante", &ckpt) == 0);

    assert(cn_checkpoint_load(tmp_dir, "Elefante", &loaded) == 0);
    assert(memcmp(ckpt.transfer_id, loaded.transfer_id, CN_TRANSFER_ID_SIZE) == 0);
    assert(strcmp(ckpt.topic, loaded.topic) == 0);
    assert(memcmp(ckpt.manifest_identity, loaded.manifest_identity, CN_SHA256_SIZE) == 0);
    assert(ckpt.file_index == loaded.file_index);
    assert(ckpt.contiguous_offset == loaded.contiguous_offset);
    assert(loaded.completed_count == 2);
    assert(loaded.completed[0].file_index == 0);
    assert(memcmp(loaded.completed[0].sha256, file_hash1, CN_SHA256_SIZE) == 0);
    assert(loaded.completed[1].file_index == 1);
    assert(memcmp(loaded.completed[1].sha256, file_hash2, CN_SHA256_SIZE) == 0);

    /* Test truncation of partial file */
    (void)snprintf(part_path, sizeof(part_path), "%s/.file5.part", topic_dir);
    fd = open(part_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);
    assert(write(fd, "12345678901234567890", 20) == 20);
    close(fd);

    assert(cn_checkpoint_truncate_part_file(tmp_dir, "Elefante", "file5", 10) == 0);
    assert(stat(part_path, &st) == 0);
    assert((uint64_t)st.st_size == 10);

    cn_checkpoint_remove(tmp_dir, "Elefante");
    cn_checkpoint_free(&loaded);
    memset(&loaded, 0, sizeof(loaded));
    assert(cn_checkpoint_load(tmp_dir, "Elefante", &loaded) != 0);

    cn_checkpoint_free(&ckpt);
    cn_checkpoint_free(&loaded);
    (void)unlink(part_path);
    (void)rmdir(topic_dir);
    (void)rmdir(tmp_dir);
}

int main(void)
{
    test_case2_protocol_encoding();
    test_session_store();
    test_client_checkpoint();
    printf("all Case 2 unit tests passed\n");
    return 0;
}
