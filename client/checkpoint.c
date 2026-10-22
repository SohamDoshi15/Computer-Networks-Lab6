#define _POSIX_C_SOURCE 200809L

#include "checkpoint.h"
#include "../common/serialization.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* magic, version, transfer ID, topic length, identity, file index, offset, count */
#define CHECKPOINT_FIXED_SIZE (4u + 4u + CN_TRANSFER_ID_SIZE + 4u + CN_SHA256_SIZE + 4u + 8u + 4u)
#define CHECKPOINT_ENTRY_SIZE (4u + CN_SHA256_SIZE)
/* Its completed list is sent back in RESUME, which has to fit in one frame. */
#define CHECKPOINT_MAX_SIZE CN_MAX_PAYLOAD_SIZE

static int write_all(int fd, const uint8_t *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        const ssize_t result = write(fd, data + written, length - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int get_topic_path(char *buf, size_t buf_cap, const char *output_dir,
                          const char *topic, const char *filename)
{
    int n;
    if (filename != NULL && filename[0] != '\0') {
        n = snprintf(buf, buf_cap, "%s/%s/%s", output_dir, topic, filename);
    } else {
        n = snprintf(buf, buf_cap, "%s/%s", output_dir, topic);
    }
    return (n > 0 && (size_t)n < buf_cap) ? 0 : -1;
}

int cn_checkpoint_init(struct cn_checkpoint *ckpt,
                       const uint8_t transfer_id[CN_TRANSFER_ID_SIZE],
                       const char *topic,
                       const uint8_t manifest_identity[CN_SHA256_SIZE])
{
    if (ckpt == NULL || transfer_id == NULL || topic == NULL || manifest_identity == NULL) {
        return -1;
    }
    memset(ckpt, 0, sizeof(*ckpt));
    memcpy(ckpt->transfer_id, transfer_id, CN_TRANSFER_ID_SIZE);
    (void)snprintf(ckpt->topic, sizeof(ckpt->topic), "%s", topic);
    memcpy(ckpt->manifest_identity, manifest_identity, CN_SHA256_SIZE);
    ckpt->file_index = 0;
    ckpt->contiguous_offset = 0;
    ckpt->completed_count = 0;
    ckpt->completed = NULL;
    return 0;
}

int cn_checkpoint_add_completed(struct cn_checkpoint *ckpt,
                                uint32_t file_index,
                                const uint8_t sha256[CN_SHA256_SIZE])
{
    struct cn_completed_file_entry *new_arr;
    uint32_t i;

    if (ckpt == NULL || sha256 == NULL) return -1;

    for (i = 0; i < ckpt->completed_count; ++i) {
        if (ckpt->completed[i].file_index == file_index) {
            memcpy(ckpt->completed[i].sha256, sha256, CN_SHA256_SIZE);
            return 0;
        }
    }

    new_arr = realloc(ckpt->completed, (ckpt->completed_count + 1) * sizeof(*new_arr));
    if (new_arr == NULL) return -1;
    ckpt->completed = new_arr;
    ckpt->completed[ckpt->completed_count].file_index = file_index;
    memcpy(ckpt->completed[ckpt->completed_count].sha256, sha256, CN_SHA256_SIZE);
    ckpt->completed_count += 1;
    return 0;
}

int cn_checkpoint_save(const char *output_dir, const char *topic,
                       const struct cn_checkpoint *ckpt)
{
    char tmp_path[1024];
    char final_path[1024];
    char dir_path[1024];
    uint8_t *buffer;
    size_t capacity;
    struct cn_writer writer;
    uint32_t i;
    int fd, dir_fd, result = -1;

    if (output_dir == NULL || topic == NULL || ckpt == NULL) return -1;
    if (get_topic_path(tmp_path, sizeof(tmp_path), output_dir, topic, ".checkpoint.tmp") != 0 ||
        get_topic_path(final_path, sizeof(final_path), output_dir, topic, ".checkpoint") != 0 ||
        get_topic_path(dir_path, sizeof(dir_path), output_dir, topic, "") != 0) {
        return -1;
    }

    /* The completed list grows with the topic (Animals-10 topics hold up to
       ~5000 files), so size the buffer from it instead of a fixed array. */
    capacity = CHECKPOINT_FIXED_SIZE + strlen(ckpt->topic) +
               (size_t)ckpt->completed_count * CHECKPOINT_ENTRY_SIZE;
    if (capacity > CHECKPOINT_MAX_SIZE) return -1;
    buffer = malloc(capacity);
    if (buffer == NULL) return -1;

    cn_writer_init(&writer, buffer, capacity);
    if (cn_writer_put_u32(&writer, CN_CHECKPOINT_MAGIC) != 0 ||
        cn_writer_put_u32(&writer, CN_CHECKPOINT_VERSION) != 0 ||
        cn_writer_put_bytes(&writer, ckpt->transfer_id, CN_TRANSFER_ID_SIZE) != 0 ||
        cn_writer_put_string(&writer, ckpt->topic, CN_MAX_TOPIC_SIZE) != 0 ||
        cn_writer_put_bytes(&writer, ckpt->manifest_identity, CN_SHA256_SIZE) != 0 ||
        cn_writer_put_u32(&writer, ckpt->file_index) != 0 ||
        cn_writer_put_u64(&writer, ckpt->contiguous_offset) != 0 ||
        cn_writer_put_u32(&writer, ckpt->completed_count) != 0) {
        goto done;
    }

    for (i = 0; i < ckpt->completed_count; ++i) {
        if (cn_writer_put_u32(&writer, ckpt->completed[i].file_index) != 0 ||
            cn_writer_put_bytes(&writer, ckpt->completed[i].sha256, CN_SHA256_SIZE) != 0) {
            goto done;
        }
    }

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) goto done;

    if (write_all(fd, buffer, writer.offset) != 0 || fsync(fd) != 0) {
        close(fd);
        (void)unlink(tmp_path);
        goto done;
    }
    close(fd);

    if (rename(tmp_path, final_path) != 0) {
        (void)unlink(tmp_path);
        goto done;
    }

    dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        (void)fsync(dir_fd);
        close(dir_fd);
    }
    result = 0;

done:
    free(buffer);
    return result;
}

int cn_checkpoint_load(const char *output_dir, const char *topic,
                       struct cn_checkpoint *ckpt)
{
    char path[1024];
    uint8_t *buffer = NULL;
    struct cn_reader reader;
    struct stat st;
    uint32_t magic, version, i;
    size_t length = 0;
    int fd, result = -1;

    if (output_dir == NULL || topic == NULL || ckpt == NULL) return -1;
    memset(ckpt, 0, sizeof(*ckpt));

    if (get_topic_path(path, sizeof(path), output_dir, topic, ".checkpoint") != 0) {
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > CHECKPOINT_MAX_SIZE) {
        close(fd);
        return -1;
    }
    buffer = malloc((size_t)st.st_size);
    if (buffer == NULL) {
        close(fd);
        return -1;
    }
    while (length < (size_t)st.st_size) {
        const ssize_t got = read(fd, buffer + length, (size_t)st.st_size - length);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        length += (size_t)got;
    }
    close(fd);
    if (length != (size_t)st.st_size) goto done;

    cn_reader_init(&reader, buffer, length);
    if (cn_reader_get_u32(&reader, &magic) != 0 || magic != CN_CHECKPOINT_MAGIC ||
        cn_reader_get_u32(&reader, &version) != 0 || version != CN_CHECKPOINT_VERSION ||
        cn_reader_get_bytes(&reader, ckpt->transfer_id, CN_TRANSFER_ID_SIZE) != 0 ||
        cn_reader_get_string(&reader, ckpt->topic, sizeof(ckpt->topic), CN_MAX_TOPIC_SIZE) != 0 ||
        cn_reader_get_bytes(&reader, ckpt->manifest_identity, CN_SHA256_SIZE) != 0 ||
        cn_reader_get_u32(&reader, &ckpt->file_index) != 0 ||
        cn_reader_get_u64(&reader, &ckpt->contiguous_offset) != 0 ||
        cn_reader_get_u32(&reader, &ckpt->completed_count) != 0 ||
        cn_reader_remaining(&reader) != (size_t)ckpt->completed_count * CHECKPOINT_ENTRY_SIZE) {
        goto done;
    }

    if (ckpt->completed_count > 0) {
        ckpt->completed = calloc(ckpt->completed_count, sizeof(*ckpt->completed));
        if (ckpt->completed == NULL) goto done;

        for (i = 0; i < ckpt->completed_count; ++i) {
            if (cn_reader_get_u32(&reader, &ckpt->completed[i].file_index) != 0 ||
                cn_reader_get_bytes(&reader, ckpt->completed[i].sha256, CN_SHA256_SIZE) != 0) {
                goto done;
            }
        }
    }
    result = 0;

done:
    if (result != 0) {
        cn_checkpoint_free(ckpt);
    }
    free(buffer);
    return result;
}

void cn_checkpoint_remove(const char *output_dir, const char *topic)
{
    char path[1024];
    if (get_topic_path(path, sizeof(path), output_dir, topic, ".checkpoint") == 0) {
        (void)unlink(path);
    }
    if (get_topic_path(path, sizeof(path), output_dir, topic, ".checkpoint.tmp") == 0) {
        (void)unlink(path);
    }
}

void cn_checkpoint_free(struct cn_checkpoint *ckpt)
{
    if (ckpt != NULL) {
        free(ckpt->completed);
        ckpt->completed = NULL;
        ckpt->completed_count = 0;
    }
}

int cn_checkpoint_truncate_part_file(const char *output_dir, const char *topic,
                                     const char *file_name, uint64_t target_offset)
{
    char part_name[1024];
    char full_path[2048];
    struct stat st;
    int fd;

    if (output_dir == NULL || topic == NULL || file_name == NULL) return -1;
    (void)snprintf(part_name, sizeof(part_name), ".%s.part", file_name);
    if (get_topic_path(full_path, sizeof(full_path), output_dir, topic, part_name) != 0) {
        return -1;
    }

    if (stat(full_path, &st) != 0) {
        /* File does not exist yet; nothing to truncate */
        return 0;
    }

    if ((uint64_t)st.st_size > target_offset) {
        fd = open(full_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) return -1;
        if (ftruncate(fd, (off_t)target_offset) != 0) {
            close(fd);
            return -1;
        }
        (void)fsync(fd);
        close(fd);
    }
    return 0;
}
