#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define SPARSE_MAGIC "SPARSE01"

static int write_all(int fd, const uint8_t *buf, size_t count, off_t offset) {
    size_t written = 0;
    while (written < count) {
        ssize_t ret = pwrite(fd, buf + written, count - written, offset + (off_t)written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("pwrite");
            return -1;
        }
        if (ret == 0) {
            fprintf(stderr, "Error: zero bytes written by pwrite\n");
            return -1;
        }
        written += (size_t)ret;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output_file>\n", argv[0]);
        return 1;
    }
    const char *out_path = argv[1];
    if (strlen(out_path) == 0) {
        fprintf(stderr, "Error: output path cannot be empty\n");
        return 1;
    }

    char magic[8];
    if (fread(magic, 1, 8, stdin) != 8 || memcmp(magic, SPARSE_MAGIC, 8) != 0) {
        fprintf(stderr, "Error: Invalid sparse stream header (expected '%s')\n", SPARSE_MAGIC);
        return 1;
    }

    uint64_t total_size = 0;
    if (fread(&total_size, 1, 8, stdin) != 8) {
        fprintf(stderr, "Error: Failed to read total size from stream\n");
        return 1;
    }

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open output file");
        return 1;
    }

    if (ftruncate(fd, (off_t)total_size) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    uint8_t buf[65536];
    uint64_t chunks_count = 0;
    uint64_t bytes_count = 0;

    while (1) {
        uint64_t chunk_offset;
        uint32_t chunk_len;

        size_t n = fread(&chunk_offset, 1, 8, stdin);
        if (n == 0) break; /* Clean EOF */
        if (n != 8) {
            fprintf(stderr, "Error: Incomplete chunk offset read\n");
            close(fd);
            return 1;
        }
        if (chunk_offset == 0xFFFFFFFFFFFFFFFFULL) break; /* End of stream marker */

        if (fread(&chunk_len, 1, 4, stdin) != 4) {
            fprintf(stderr, "Error: Incomplete chunk length read\n");
            close(fd);
            return 1;
        }

        uint32_t rem = chunk_len;
        off_t cur_off = (off_t)chunk_offset;

        while (rem > 0) {
            size_t to_read = (rem > sizeof(buf)) ? sizeof(buf) : rem;
            if (fread(buf, 1, to_read, stdin) != to_read) {
                fprintf(stderr, "Error: Incomplete chunk data read\n");
                close(fd);
                return 1;
            }
            if (write_all(fd, buf, to_read, cur_off) != 0) {
                close(fd);
                return 1;
            }
            cur_off += (off_t)to_read;
            rem -= (uint32_t)to_read;
        }

        chunks_count++;
        bytes_count += chunk_len;
    }

    fsync(fd);
    close(fd);

    fprintf(stderr, "[✓] Unpacked %llu sparse chunks (%llu bytes) into %s (virtual size %llu bytes)\n",
            (unsigned long long)chunks_count, (unsigned long long)bytes_count,
            out_path, (unsigned long long)total_size);
    return 0;
}
