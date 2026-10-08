#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/file.h>
#include <errno.h>

/* Safe little-endian accessors avoiding unaligned memory access traps on ARMv7 */
static inline uint16_t get_le16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t get_le32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint64_t get_le64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline void put_le16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}

static inline void put_le32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static inline void put_le64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

/* NTFS Update Sequence Array (USA / Fixup) handling */
static int ntfs_apply_fixup(uint8_t *rec, uint32_t rec_size) {
    if (memcmp(rec, "FILE", 4) != 0) return -1;
    uint16_t usa_off = get_le16(rec + 4);
    uint16_t usa_cnt = get_le16(rec + 6);
    if ((uint32_t)usa_off + (uint32_t)usa_cnt * 2 > rec_size) return -1;

    uint16_t usn = get_le16(rec + usa_off);
    for (uint16_t i = 1; i < usa_cnt; i++) {
        uint32_t sect_end = (uint32_t)i * 512;
        if (sect_end > rec_size) break;
        uint16_t val = get_le16(rec + sect_end - 2);
        if (val != usn) {
            fprintf(stderr, "[!] Warning: MFT sector %u USN mismatch (val=0x%04x, usn=0x%04x)\n", i, val, usn);
        }
        uint16_t orig = get_le16(rec + usa_off + i * 2);
        put_le16(rec + sect_end - 2, orig);
    }
    return 0;
}

static int ntfs_write_fixup(uint8_t *rec, uint32_t rec_size) {
    if (memcmp(rec, "FILE", 4) != 0) return -1;
    uint16_t usa_off = get_le16(rec + 4);
    uint16_t usa_cnt = get_le16(rec + 6);
    if ((uint32_t)usa_off + (uint32_t)usa_cnt * 2 > rec_size) return -1;

    uint16_t usn = get_le16(rec + usa_off);
    usn++;
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    put_le16(rec + usa_off, usn);

    for (uint16_t i = 1; i < usa_cnt; i++) {
        uint32_t sect_end = (uint32_t)i * 512;
        if (sect_end > rec_size) break;
        uint16_t orig = get_le16(rec + sect_end - 2);
        put_le16(rec + usa_off + i * 2, orig);
        put_le16(rec + sect_end - 2, usn);
    }
    return 0;
}

static void to_utf16(const char *src, uint8_t *dst, size_t max_bytes) {
    size_t len = strlen(src);
    for (size_t i = 0; i < len && (i * 2 + 2) <= max_bytes; i++) {
        dst[i * 2] = (uint8_t)src[i];
        dst[i * 2 + 1] = 0;
    }
}

int main(int argc, char *argv[]) {
    const char *path = "/data/local/tmp/ntfs_lab/ntfs_template.bin";
    const char *force_mode = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            force_mode = argv[i];
        } else {
            path = argv[i];
        }
    }

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* File lock to prevent concurrent modifications */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "[!] Warning: could not obtain exclusive flock on %s (busy?)\n", path);
    }

    /* Target names to alternate between */
    const char *name_a = "TV AO VIVO.trp";
    const char *name_b = "TV AO VIVO 2.tp";

    uint8_t name_a_u16[32];
    uint8_t name_b_u16[32];
    memset(name_a_u16, 0, sizeof(name_a_u16));
    memset(name_b_u16, 0, sizeof(name_b_u16));
    to_utf16(name_a, name_a_u16, sizeof(name_a_u16));
    to_utf16(name_b, name_b_u16, sizeof(name_b_u16));
    size_t name_a_bytes = strlen(name_a) * 2;
    size_t name_b_bytes = strlen(name_b) * 2;

    /* 1. Parse VBR (Sector 0) */
    uint8_t vbr[512];
    if (pread(fd, vbr, sizeof(vbr), 0) != sizeof(vbr)) {
        perror("pread VBR");
        close(fd);
        return 1;
    }

    if (memcmp(vbr + 3, "NTFS    ", 8) != 0) {
        fprintf(stderr, "[!] Warning: VBR OEM ID is not 'NTFS    '\n");
    }

    uint16_t bps = get_le16(vbr + 11);
    uint8_t spc = vbr[13];
    if (bps != 512 && bps != 4096) bps = 512;
    if (spc == 0) spc = 8;

    uint64_t cluster_size = (uint64_t)bps * (uint64_t)spc;
    uint64_t mft_lcn = get_le64(vbr + 48);
    int8_t mft_rec_code = (int8_t)vbr[64];
    uint32_t rec_size = (mft_rec_code < 0) ? (1U << (-mft_rec_code)) : ((uint32_t)mft_rec_code * (uint32_t)cluster_size);
    if (rec_size == 0 || rec_size > 4096) rec_size = 1024;
    uint64_t mft_base = mft_lcn * cluster_size;

    printf("[*] NTFS VBR: BPS=%u SPC=%u cluster_sz=%" PRIu64 " mft_lcn=%" PRIu64 " rec_sz=%u mft_base=%" PRIu64 "\n",
           bps, spc, cluster_size, mft_lcn, rec_size, mft_base);

    /* 2. Dynamically scan MFT records for TV AO VIVO */
    int target_inode = -1;
    uint64_t target_mft_off = 0;
    uint8_t mft[4096];
    int current_is_b = 0;

    for (int i = 0; i < 256; i++) {
        uint64_t off = mft_base + (uint64_t)i * rec_size;
        if (pread(fd, mft, rec_size, (off_t)off) != (ssize_t)rec_size) break;
        if (memcmp(mft, "FILE", 4) != 0) continue;

        ntfs_apply_fixup(mft, rec_size);

        uint16_t attr_off = get_le16(mft + 20);
        if (attr_off < 42 || (uint32_t)attr_off + 8 > rec_size) continue;

        uint32_t cur = attr_off;
        while (cur + 8 <= rec_size) {
            uint32_t attr_type = get_le32(mft + cur);
            uint32_t attr_len = get_le32(mft + cur + 4);
            if (attr_type == 0xFFFFFFFF || attr_len == 0 || cur + attr_len > rec_size) break;

            if (attr_type == 0x30 && mft[cur + 8] == 0) { /* Resident $FILE_NAME */
                uint16_t val_off = get_le16(mft + cur + 20);
                uint32_t fn_data = cur + val_off;
                if (fn_data + 66 <= cur + attr_len) {
                    uint8_t fn_len = mft[fn_data + 64];
                    uint8_t *fn_name = mft + fn_data + 66;
                    size_t fn_bytes = (size_t)fn_len * 2;
                    if (fn_data + 66 + fn_bytes <= cur + attr_len) {
                        if (fn_bytes == name_b_bytes && memcmp(fn_name, name_b_u16, fn_bytes) == 0) {
                            target_inode = i;
                            target_mft_off = off;
                            current_is_b = 1;
                            break;
                        } else if (fn_bytes == name_a_bytes && memcmp(fn_name, name_a_u16, fn_bytes) == 0) {
                            target_inode = i;
                            target_mft_off = off;
                            current_is_b = 0;
                            break;
                        }
                    }
                }
            }
            cur += attr_len;
        }
        if (target_inode >= 0) break;
    }

    if (target_inode < 0) {
        fprintf(stderr, "[!] Target file '%s' or '%s' not found in MFT\n", name_a, name_b);
        close(fd);
        return 1;
    }

    const char *target_name = name_b;
    if (force_mode && strcmp(force_mode, "--tp") == 0) {
        target_name = name_b;
    } else if (force_mode && strcmp(force_mode, "--trp") == 0) {
        target_name = name_a;
    } else {
        target_name = current_is_b ? name_a : name_b;
    }

    printf("[*] Found stream file at Inode %d (offset %" PRIu64 "). Alternating to '%s'\n",
           target_inode, target_mft_off, target_name);

    size_t target_len = strlen(target_name);
    uint8_t target_utf16[32];
    memset(target_utf16, 0, sizeof(target_utf16));
    to_utf16(target_name, target_utf16, sizeof(target_utf16));
    size_t target_bytes = target_len * 2;
    uint16_t val_len = (uint16_t)(66 + target_bytes);

    /* 3. Patch MFT Record with USA/Fixup */
    if (pread(fd, mft, rec_size, (off_t)target_mft_off) != (ssize_t)rec_size) {
        perror("pread target mft");
        close(fd);
        return 1;
    }

    ntfs_apply_fixup(mft, rec_size);

    uint16_t attr_off = get_le16(mft + 20);
    uint32_t cur = attr_off;
    int patched_mft = 0;

    while (cur + 8 <= rec_size) {
        uint32_t attr_type = get_le32(mft + cur);
        uint32_t attr_len = get_le32(mft + cur + 4);
        if (attr_type == 0xFFFFFFFF || attr_len == 0 || cur + attr_len > rec_size) break;

        if (attr_type == 0x30 && mft[cur + 8] == 0) {
            uint16_t val_off = get_le16(mft + cur + 20);
            put_le32(mft + cur + 16, val_len);

            uint32_t fn_data = cur + val_off;
            mft[fn_data + 64] = (uint8_t)target_len;
            memcpy(mft + fn_data + 66, target_utf16, target_bytes);

            uint32_t pad_start = fn_data + 66 + (uint32_t)target_bytes;
            uint32_t pad_end = cur + attr_len;
            if (pad_end > pad_start) {
                memset(mft + pad_start, 0, pad_end - pad_start);
            }
            patched_mft = 1;
            break;
        }
        cur += attr_len;
    }

    if (!patched_mft) {
        fprintf(stderr, "[!] Failed to locate $FILE_NAME in target MFT record\n");
        close(fd);
        return 1;
    }

    ntfs_write_fixup(mft, rec_size);

    if (pwrite(fd, mft, rec_size, (off_t)target_mft_off) != (ssize_t)rec_size) {
        perror("pwrite mft");
        close(fd);
        return 1;
    }
    printf("[✓] Inode %d patched to '%s'\n", target_inode, target_name);

    /* 4. Dynamically find and patch Root Directory Index entry */
    uint8_t inode_ref[4];
    inode_ref[0] = (uint8_t)(target_inode & 0xFF);
    inode_ref[1] = (uint8_t)((target_inode >> 8) & 0xFF);
    inode_ref[2] = 0;
    inode_ref[3] = 0;

    uint8_t needle_prefix[32];
    memset(needle_prefix, 0, sizeof(needle_prefix));
    to_utf16("TV AO VIVO", needle_prefix, sizeof(needle_prefix));
    size_t needle_bytes = 20; /* 10 chars * 2 */

    uint64_t dir_entry_off = 0;
    int found_dir_entry = 0;

    /* 4a. Check Inode 5 (Root Directory) directly via $INDEX_ROOT and $INDEX_ALLOCATION */
    uint8_t root_mft[4096];
    if (pread(fd, root_mft, rec_size, (off_t)(mft_base + 5ULL * rec_size)) == (ssize_t)rec_size &&
        memcmp(root_mft, "FILE", 4) == 0) {
        ntfs_apply_fixup(root_mft, rec_size);
        uint16_t rattr_off = get_le16(root_mft + 20);
        uint32_t rcur = rattr_off;

        while (rcur + 8 <= rec_size && !found_dir_entry) {
            uint32_t atype = get_le32(root_mft + rcur);
            uint32_t alen = get_le32(root_mft + rcur + 4);
            if (atype == 0xFFFFFFFF || alen == 0 || rcur + alen > rec_size) break;

            if (atype == 0x90 && root_mft[rcur + 8] == 0) { /* Resident $INDEX_ROOT */
                uint16_t voff = get_le16(root_mft + rcur + 20);
                uint32_t vlen = get_le32(root_mft + rcur + 16);
                if (rcur + voff + vlen <= rec_size) {
                    uint8_t *ventry = root_mft + rcur + voff;
                    for (uint32_t s = 0; s + 128 <= vlen; s++) {
                        if (ventry[s] == inode_ref[0] && ventry[s+1] == inode_ref[1] &&
                            ventry[s+2] == 0 && ventry[s+3] == 0 && ventry[s+4] == 0 && ventry[s+5] == 0) {
                            if (s + 82 + needle_bytes <= vlen &&
                                memcmp(ventry + s + 82, needle_prefix, needle_bytes) == 0) {
                                dir_entry_off = mft_base + 5ULL * rec_size + (uint64_t)(rcur + voff + s);
                                found_dir_entry = 1;
                                break;
                            }
                        }
                    }
                }
            } else if (atype == 0xA0 && root_mft[rcur + 8] != 0) { /* Non-resident $INDEX_ALLOCATION */
                uint16_t run_off = get_le16(root_mft + rcur + 32);
                if (rcur + run_off < rcur + alen) {
                    const uint8_t *p = root_mft + rcur + run_off;
                    int64_t current_lcn = 0;
                    while (*p && (p < root_mft + rcur + alen) && !found_dir_entry) {
                        uint8_t hdr = *p++;
                        int len_sz = hdr & 0x0F;
                        int off_sz = (hdr >> 4) & 0x0F;
                        if (len_sz == 0 || len_sz > 8 || off_sz > 8) break;
                        if (p + len_sz + off_sz > root_mft + rcur + alen) break;

                        int64_t run_len = 0;
                        for (int b = 0; b < len_sz; b++) run_len |= ((int64_t)*p++) << (b * 8);
                        int64_t lcn_delta = 0;
                        for (int b = 0; b < off_sz; b++) lcn_delta |= ((int64_t)*p++) << (b * 8);
                        if (off_sz > 0 && off_sz < 8 && (*(p - 1) & 0x80)) {
                            lcn_delta |= (int64_t)(~0ULL << (off_sz * 8));
                        }
                        current_lcn += lcn_delta;

                        uint64_t byte_off = (uint64_t)current_lcn * cluster_size;
                        uint64_t byte_sz = (uint64_t)run_len * cluster_size;

                        /* Chunked scanning in 1MB heap windows to avoid OOM */
                        #define SCAN_CHUNK (1024 * 1024)
                        uint8_t *buf = malloc(SCAN_CHUNK);
                        if (buf) {
                            uint64_t scanned = 0;
                            while (scanned < byte_sz && !found_dir_entry) {
                                size_t cur_to_read = (byte_sz - scanned > SCAN_CHUNK) ? SCAN_CHUNK : (size_t)(byte_sz - scanned);
                                ssize_t rd = pread(fd, buf, cur_to_read, (off_t)(byte_off + scanned));
                                if (rd <= 0) break;
                                for (size_t s = 0; s + 128 <= (size_t)rd; s++) {
                                    if (buf[s] == inode_ref[0] && buf[s+1] == inode_ref[1] &&
                                        buf[s+2] == 0 && buf[s+3] == 0 && buf[s+4] == 0 && buf[s+5] == 0) {
                                        if (s + 82 + needle_bytes <= (size_t)rd &&
                                            memcmp(buf + s + 82, needle_prefix, needle_bytes) == 0) {
                                            dir_entry_off = byte_off + scanned + (uint64_t)s;
                                            found_dir_entry = 1;
                                            break;
                                        }
                                    }
                                }
                                scanned += (cur_to_read > 512 ? cur_to_read - 512 : cur_to_read);
                            }
                            free(buf);
                        }
                    }
                }
            }
            rcur += alen;
        }
    }

    /* 4b. Fallback scan if not resolved via Inode 5 (capped at 32 MB heap-buffered) */
    if (!found_dir_entry) {
        uint8_t *chunk = malloc(1024 * 1024);
        if (chunk) {
            uint64_t max_scan = 32ULL * 1024 * 1024;
            for (uint64_t scan_pos = 0; scan_pos < max_scan; scan_pos += (1024 * 1024) - 512) {
                ssize_t rd = pread(fd, chunk, 1024 * 1024, (off_t)scan_pos);
                if (rd <= 0) break;

                for (ssize_t j = 0; j + 128 <= rd; j++) {
                    if (chunk[j] == inode_ref[0] && chunk[j+1] == inode_ref[1] &&
                        chunk[j+2] == 0 && chunk[j+3] == 0 &&
                        chunk[j+4] == 0 && chunk[j+5] == 0) {
                        uint64_t candidate_off = scan_pos + (uint64_t)j;
                        if (candidate_off >= mft_base && candidate_off < mft_base + 256ULL * rec_size) {
                            continue;
                        }
                        if (j + 82 + (ssize_t)needle_bytes <= rd &&
                            memcmp(chunk + j + 82, needle_prefix, needle_bytes) == 0) {
                            dir_entry_off = candidate_off;
                            found_dir_entry = 1;
                            break;
                        }
                    }
                }
                if (found_dir_entry) break;
            }
            free(chunk);
        }
    }

    if (!found_dir_entry) {
        fprintf(stderr, "[!] Directory entry for Inode %d not found\n", target_inode);
        close(fd);
        return 1;
    }

    printf("[*] Found directory entry at offset %" PRIu64 " (0x%" PRIx64 ")\n",
           dir_entry_off, dir_entry_off);

    uint8_t dir[256];
    if (pread(fd, dir, sizeof(dir), (off_t)dir_entry_off) != sizeof(dir)) {
        perror("pread dir");
        close(fd);
        return 1;
    }

    /* Update entry length at offset 8 (aligned to 8 bytes) and Content length at offset 10 */
    uint16_t new_entry_len = (uint16_t)(16 + val_len);
    new_entry_len = (new_entry_len + 7) & ~7;
    put_le16(dir + 8, new_entry_len);
    put_le16(dir + 10, val_len);

    /* Update Name length at offset 80 and Name bytes at offset 82 */
    dir[80] = (uint8_t)target_len;
    int dir_name_off = 82;
    memcpy(dir + dir_name_off, target_utf16, target_bytes);
    int dir_pad_start = dir_name_off + (int)target_bytes;
    int dir_pad_end = 82 + 32; /* padded to previous max name */
    if (dir_pad_end > dir_pad_start) {
        memset(dir + dir_pad_start, 0, (size_t)(dir_pad_end - dir_pad_start));
    }

    if (pwrite(fd, dir, sizeof(dir), (off_t)dir_entry_off) != sizeof(dir)) {
        perror("pwrite dir");
        close(fd);
        return 1;
    }
    printf("[✓] Directory index entry patched to '%s'\n", target_name);

    /* Ensure eMMC persistence */
    fsync(fd);
    close(fd);

    printf("[✓] Alternation complete: TV will see '%s'\n", target_name);
    return 0;
}
