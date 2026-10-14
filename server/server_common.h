#ifndef PART1_SERVER_COMMON_H
#define PART1_SERVER_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct part1_file {
    char *name;
    uint64_t size;
    uint8_t sha256[32];
};

struct part1_manifest {
    char *topic_path;
    struct part1_file *files;
    size_t count;
    uint64_t total_bytes;
};

int part1_validate_topic_name(const char *topic);
int part1_build_manifest(const char *root, const char *topic,
                         struct part1_manifest *manifest);
void part1_free_manifest(struct part1_manifest *manifest);
int part1_send_error(int client_fd, const uint8_t transfer_id[16],
                     uint32_t code, const char *message);
int part1_serve_client(int client_fd, const char *root);

#endif
