#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

_Static_assert(sizeof(off64_t) == 8, "need 64-bit offsets for thin-provisioned NTFS images");

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

static inline int io_read(int fd, void *b, size_t n, uint64_t o) {
    return pread64(fd, b, n, (off64_t)o) == (ssize_t)n ? 0 : -1;
}

static inline int io_write(int fd, const void *b, size_t n, uint64_t o) {
    return pwrite64(fd, b, n, (off64_t)o) == (ssize_t)n ? 0 : -1;
}

/* NTFS Update Sequence Array (USA / Fixup) handling for FILE and INDX records */
#define SEC 512U

static int ntfs_fixup_decode(uint8_t *rec, uint32_t size, const char magic[4]) {
    if (memcmp(rec, magic, 4) != 0) return -1;
    uint16_t off = get_le16(rec + 4);
    uint16_t cnt = get_le16(rec + 6);
    if (off < 0x28 || (off & 1) || cnt != size / SEC + 1 || (uint32_t)off + (uint32_t)cnt * 2U > size) {
        return -1;
    }
    uint16_t usn = get_le16(rec + off);
    for (uint16_t i = 1; i < cnt; i++) {
        uint8_t *tail = rec + i * SEC - 2;
        if (get_le16(tail) != usn) {
            fprintf(stderr, "[!] Torn record detected in %c%c%c%c at sector %u (USN 0x%04x != 0x%04x)\n",
                    magic[0], magic[1], magic[2], magic[3], i, get_le16(tail), usn);
            return -1; /* Strict: do NOT touch torn/corrupted records */
        }
        memcpy(tail, rec + off + i * 2, 2);
    }
    return 0;
}

static void ntfs_fixup_encode(uint8_t *rec, uint32_t size) {
    (void)size;
    uint16_t off = get_le16(rec + 4);
    uint16_t cnt = get_le16(rec + 6);
    uint16_t usn = (uint16_t)(get_le16(rec + off) + 1);
    if (usn == 0 || usn == 0xFFFF) usn = 1;
    put_le16(rec + off, usn);
    for (uint16_t i = 1; i < cnt; i++) {
        uint8_t *tail = rec + i * SEC - 2;
        memcpy(rec + off + i * 2, tail, 2);
        put_le16(tail, usn);
    }
}

/* NTFS Unicode collation helpers */
static inline uint16_t up16(uint16_t c) {
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}

static int coll_fn(const uint8_t *a, int an, const uint8_t *b, int bn) {
    int n = (an < bn) ? an : bn;
    for (int i = 0; i < n; i++) {
        uint16_t x = up16(get_le16(a + 2 * i));
        uint16_t y = up16(get_le16(b + 2 * i));
        if (x != y) return (x < y) ? -1 : 1;
    }
    return (an > bn) - (an < bn);
}

static void to_utf16(const char *src, uint8_t *dst, size_t max_bytes) {
    size_t len = strlen(src);
    for (size_t i = 0; i < len && (i * 2 + 2) <= max_bytes; i++) {
        dst[i * 2] = (uint8_t)src[i];
        dst[i * 2 + 1] = 0;
    }
}

/* Parse NTFS non-resident data runs safely avoiding undefined behavior */
static int next_run(const uint8_t **pp, const uint8_t *end, int64_t *lcn, uint64_t *len, int *sparse) {
    const uint8_t *p = *pp;
    if (p >= end || *p == 0) return 0;
    uint8_t h = *p++;
    unsigned ls = h & 0x0F;
    unsigned os = h >> 4;
    if (!ls || ls > 8 || os > 8 || (size_t)(end - p) < (size_t)(ls + os)) return -1;

    uint64_t l = 0;
    for (unsigned b = 0; b < ls; b++) l |= ((uint64_t)*p++) << (8 * b);
    if (!l || (l >> 62)) return -1;

    *sparse = (os == 0);
    if (os > 0) {
        uint64_t d = 0;
        for (unsigned b = 0; b < os; b++) d |= ((uint64_t)*p++) << (8 * b);
        if (os < 8 && ((d >> (os * 8 - 1)) & 1)) {
            d |= ~0ULL << (os * 8);
        }
        int64_t sd = (int64_t)d;
        if ((sd > 0 && *lcn > INT64_MAX - sd) || (sd < 0 && *lcn < INT64_MIN - sd)) return -1;
        *lcn += sd;
        if (*lcn < 0) return -1;
    }
    *len = l;
    *pp = p;
    return 1;
}

/* Walks an INDEX_NODE_HEADER (Root: value+16 | INDX: blk+0x18).
 * Returns:
 *   0 = matched and validated (and mutated if commit == 1)
 *  -1 = target entry not found in this node
 *  -2 = collation order violation (unless force_order == 1)
 *  -3 = layout mismatch (entry size would need to change)
 *  -4 = corrupted index node
 */
static int index_rename(uint8_t *hdr, uint8_t *limit, uint64_t target_ino, uint16_t target_seq,
                        const uint8_t *new_u16, uint8_t new_chars, int commit, int force_order)
{
    if (hdr + 16 > limit) return -4;
    uint32_t eoff = get_le32(hdr);
    uint32_t ilen = get_le32(hdr + 4);
    uint32_t alen = get_le32(hdr + 8);
    if (eoff < 16 || eoff > ilen || ilen > alen || hdr + ilen > limit) return -4;

    uint8_t *p = hdr + eoff;
    uint8_t *end = hdr + ilen;
    uint8_t *prev = NULL;

    while (p + 16 <= end) {
        uint16_t elen = get_le16(p + 8);
        uint16_t klen = get_le16(p + 10);
        uint32_t fl = get_le32(p + 12);

        if (elen < 16 || (elen & 7) || p + elen > end) return -4;
        if (fl & 2) break; /* End marker */

        if (klen >= 66 && 16U + (uint32_t)klen <= (uint32_t)elen &&
            (get_le64(p) & 0xFFFFFFFFFFFFULL) == target_ino &&
            get_le16(p + 6) == target_seq &&
            (get_le64(p + 16) & 0xFFFFFFFFFFFFULL) == 5ULL) { /* Parent is root (Inode 5) */

            uint8_t *n = p + elen;
            if (n + 16 > end || (get_le32(n + 12) & 2)) n = NULL;

            /* Check collation against prev and next */
            if (prev && coll_fn(prev + 82, prev[80], new_u16, new_chars) >= 0) {
                if (!force_order) return -2;
            }
            if (n && coll_fn(new_u16, new_chars, n + 82, n[80]) >= 0) {
                if (!force_order) return -2;
            }

            /* Check layout compatibility (C1) */
            uint16_t nklen = (uint16_t)(66 + (uint32_t)new_chars * 2);
            uint16_t need = (uint16_t)(((16 + (uint32_t)nklen + 7) & ~7) + ((fl & 1) ? 8 : 0));
            if (need != elen) return -3;

            if (commit) {
                put_le16(p + 10, nklen);
                p[80] = new_chars;
                memcpy(p + 82, new_u16, (size_t)new_chars * 2);

                size_t used = 82 + (size_t)new_chars * 2;
                size_t tail = (size_t)elen - ((fl & 1) ? 8 : 0); /* Do NOT overwrite sub-node VCN */
                if (tail > used) {
                    memset(p + used, 0, tail - used); /* Zero ONLY this entry's padding */
                }
            }
            return 0;
        }

        prev = p;
        p += elen;
    }
    return -1;
}

int main(int argc, char *argv[]) {
    const char *path = "/data/local/tmp/ntfs_lab/ntfs_template.bin";
    const char *force_mode = NULL;
    int force_order = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force-order") == 0) {
            force_order = 1;
        } else if (argv[i][0] == '-') {
            force_mode = argv[i];
        } else {
            path = argv[i];
        }
    }

    int fd = open(path, O_RDWR | O_LARGEFILE);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Exclusive lock to prevent concurrent modifications */
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "[!] Erro: Nao foi possivel obter flock exclusivo em %s (processo concorrente ativo?)\n", path);
        close(fd);
        return 75;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    uint64_t img_size = (uint64_t)st.st_size;

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

    /* 1. Parse and strictly validate VBR (Sector 0) */
    uint8_t vbr[512];
    if (io_read(fd, vbr, sizeof(vbr), 0) != 0) {
        perror("io_read VBR");
        close(fd);
        return 1;
    }

    if (vbr[510] != 0x55 || vbr[511] != 0xAA || memcmp(vbr + 3, "NTFS    ", 8) != 0) {
        fprintf(stderr, "[!] Erro: VBR invalida ou OEM ID diferente de 'NTFS    '\n");
        close(fd);
        return 1;
    }

    uint16_t bps = get_le16(vbr + 11);
    uint8_t spc = vbr[13];
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        fprintf(stderr, "[!] Erro: BPS invalido (%u)\n", bps);
        close(fd);
        return 1;
    }
    if (spc == 0) {
        fprintf(stderr, "[!] Erro: SPC invalido (0)\n");
        close(fd);
        return 1;
    }

    uint64_t cluster_size = (uint64_t)bps * (uint64_t)spc;
    uint64_t mft_lcn = get_le64(vbr + 48);
    int8_t mft_rec_code = (int8_t)vbr[64];
    uint32_t rec_size = (mft_rec_code < 0) ? (1U << (-mft_rec_code)) : ((uint32_t)mft_rec_code * (uint32_t)cluster_size);
    if (rec_size < 512 || rec_size > 4096) {
        fprintf(stderr, "[!] Erro: MFT record size invalido (%u)\n", rec_size);
        close(fd);
        return 1;
    }

    int8_t ic = (int8_t)vbr[68];
    uint32_t idx_sz = (ic > 0) ? ((uint32_t)ic * (uint32_t)cluster_size) : (ic > -17 ? (1U << (-ic)) : 0);
    if (idx_sz < 512 || idx_sz > 65536) {
        idx_sz = 4096; /* Padrao NTFS seguro */
    }

    uint64_t mft_base = mft_lcn * cluster_size;
    printf("[*] NTFS VBR: BPS=%u SPC=%u cluster_sz=%" PRIu64 " mft_lcn=%" PRIu64 " rec_sz=%u idx_sz=%u mft_base=%" PRIu64 "\n",
           bps, spc, cluster_size, mft_lcn, rec_size, idx_sz, mft_base);

    /* 2. Dynamically scan MFT records for target file */
    int target_inode = -1;
    uint16_t target_seq = 0;
    uint64_t target_mft_off = 0;
    uint32_t target_fn_attr_off = 0;
    uint8_t mft[4096];
    int current_is_b = 0;

    for (int i = 0; i < 256; i++) {
        uint64_t off = mft_base + (uint64_t)i * rec_size;
        if (io_read(fd, mft, rec_size, off) != 0) break;
        if (memcmp(mft, "FILE", 4) != 0) continue;

        if (ntfs_fixup_decode(mft, rec_size, "FILE") < 0) continue;

        /* Validate record in-use and is base record (offset 32 == 0) */
        if (!(get_le16(mft + 22) & 1) || get_le64(mft + 32) != 0) continue;

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
                            target_seq = get_le16(mft + 16);
                            target_mft_off = off;
                            target_fn_attr_off = cur;
                            current_is_b = 1;
                            break;
                        } else if (fn_bytes == name_a_bytes && memcmp(fn_name, name_a_u16, fn_bytes) == 0) {
                            target_inode = i;
                            target_seq = get_le16(mft + 16);
                            target_mft_off = off;
                            target_fn_attr_off = cur;
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
        fprintf(stderr, "[!] Erro: Arquivo de stream '%s' ou '%s' nao encontrado na MFT\n", name_a, name_b);
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

    printf("[*] Localizado arquivo no Inode %d (seq=%u, offset=%" PRIu64 "). Alternando para '%s'\n",
           target_inode, target_seq, target_mft_off, target_name);

    size_t target_len = strlen(target_name);
    uint8_t target_utf16[32];
    memset(target_utf16, 0, sizeof(target_utf16));
    to_utf16(target_name, target_utf16, sizeof(target_utf16));
    size_t target_bytes = target_len * 2;
    uint16_t val_len = (uint16_t)(66 + target_bytes);

    /* 3. Dry-run Validation on Root Directory Index (Phase 1: atomic check) */
    uint8_t root_mft[4096];
    if (io_read(fd, root_mft, rec_size, mft_base + 5ULL * rec_size) != 0 ||
        ntfs_fixup_decode(root_mft, rec_size, "FILE") < 0) {
        fprintf(stderr, "[!] Erro ao decodificar Inode 5 (Diretorio Raiz)\n");
        close(fd);
        return 1;
    }

    int found_in_root_attr = 0;
    int found_in_alloc = 0;
    uint64_t found_alloc_blk_off = 0;

    uint16_t rattr_off = get_le16(root_mft + 20);
    uint32_t rcur = rattr_off;

    while (rcur + 8 <= rec_size && !found_in_root_attr && !found_in_alloc) {
        uint32_t atype = get_le32(root_mft + rcur);
        uint32_t alen = get_le32(root_mft + rcur + 4);
        if (atype == 0xFFFFFFFF || alen == 0 || rcur + alen > rec_size) break;

        if (atype == 0x90 && root_mft[rcur + 8] == 0) { /* Resident $INDEX_ROOT */
            uint16_t voff = get_le16(root_mft + rcur + 20);
            uint32_t vlen = get_le32(root_mft + rcur + 16);
            if ((uint32_t)voff + 16 <= vlen && rcur + voff + vlen <= rec_size) {
                int r = index_rename(root_mft + rcur + voff + 16, root_mft + rcur + voff + vlen,
                                     (uint64_t)target_inode, target_seq,
                                     target_utf16, (uint8_t)target_len, /*commit=*/0, force_order);
                if (r == 0) {
                    found_in_root_attr = 1;
                    break;
                } else if (r == -2) {
                    fprintf(stderr, "[!] Erro de Collation: A ordem alfabetica da B-Tree seria violada em $INDEX_ROOT (use --force-order para ignorar)\n");
                    close(fd);
                    return 2;
                } else if (r == -3) {
                    fprintf(stderr, "[!] Erro de Layout: Tamanho da entrada no indice incompativel em $INDEX_ROOT\n");
                    close(fd);
                    return 3;
                }
            }
        } else if (atype == 0xA0 && root_mft[rcur + 8] != 0) { /* Non-resident $INDEX_ALLOCATION */
            uint16_t run_off = get_le16(root_mft + rcur + 32);
            if (rcur + run_off < rcur + alen) {
                const uint8_t *p = root_mft + rcur + run_off;
                const uint8_t *end_runs = root_mft + rcur + alen;
                int64_t current_lcn = 0;
                uint64_t run_len = 0;
                int sparse = 0;

                while (!found_in_alloc) {
                    int nret = next_run(&p, end_runs, &current_lcn, &run_len, &sparse);
                    if (nret <= 0) break;
                    if (sparse) continue;

                    if ((uint64_t)current_lcn * cluster_size >= img_size) break;
                    uint64_t byte_off = (uint64_t)current_lcn * cluster_size;
                    uint64_t byte_sz = run_len * cluster_size;
                    if (byte_off + byte_sz > img_size) byte_sz = img_size - byte_off;

                    uint8_t *blk = malloc(idx_sz);
                    if (!blk) {
                        fprintf(stderr, "[!] Erro de memoria ao alocar buffer de bloco INDX (%u bytes)\n", idx_sz);
                        close(fd);
                        return 1;
                    }

                    for (uint64_t b = 0; b + idx_sz <= byte_sz; b += idx_sz) {
                        uint64_t blk_off = byte_off + b;
                        if (io_read(fd, blk, idx_sz, blk_off) != 0) continue;
                        if (memcmp(blk, "INDX", 4) != 0) continue;
                        if (ntfs_fixup_decode(blk, idx_sz, "INDX") != 0) continue;

                        int r = index_rename(blk + 0x18, blk + idx_sz,
                                             (uint64_t)target_inode, target_seq,
                                             target_utf16, (uint8_t)target_len, /*commit=*/0, force_order);
                        if (r == 0) {
                            found_in_alloc = 1;
                            found_alloc_blk_off = blk_off;
                            free(blk);
                            break;
                        } else if (r == -2) {
                            fprintf(stderr, "[!] Erro de Collation: A ordem alfabetica da B-Tree seria violada em INDX (use --force-order para ignorar)\n");
                            free(blk);
                            close(fd);
                            return 2;
                        } else if (r == -3) {
                            fprintf(stderr, "[!] Erro de Layout: Tamanho da entrada no indice incompativel em INDX\n");
                            free(blk);
                            close(fd);
                            return 3;
                        }
                    }
                    if (!found_in_alloc) free(blk);
                }
            }
        }
        rcur += alen;
    }

    if (!found_in_root_attr && !found_in_alloc) {
        fprintf(stderr, "[!] Erro: Entrada de diretorio para Inode %d nao encontrada no indice raiz\n", target_inode);
        close(fd);
        return 1;
    }

    /* 4. Phase 2 (Commit): Both MFT and Directory Index are validated; apply mutations */

    /* 4a. Commit MFT Record */
    if (io_read(fd, mft, rec_size, target_mft_off) != 0 ||
        ntfs_fixup_decode(mft, rec_size, "FILE") < 0) {
        fprintf(stderr, "[!] Erro ao reler/decodificar registro MFT %d para commit\n", target_inode);
        close(fd);
        return 1;
    }

    uint32_t fn_cur = target_fn_attr_off;
    uint32_t fn_attr_len = get_le32(mft + fn_cur + 4);
    uint16_t fn_val_off = get_le16(mft + fn_cur + 20);

    put_le32(mft + fn_cur + 16, val_len);
    uint32_t fn_data = fn_cur + fn_val_off;
    mft[fn_data + 64] = (uint8_t)target_len;
    memcpy(mft + fn_data + 66, target_utf16, target_bytes);

    uint32_t pad_start = fn_data + 66 + (uint32_t)target_bytes;
    uint32_t pad_end = fn_cur + fn_attr_len;
    if (pad_end > pad_start) {
        memset(mft + pad_start, 0, pad_end - pad_start);
    }

    ntfs_fixup_encode(mft, rec_size);
    if (io_write(fd, mft, rec_size, target_mft_off) != 0) {
        perror("io_write mft");
        close(fd);
        return 1;
    }
    printf("[✓] Inode %d MFT record atualizado para '%s'\n", target_inode, target_name);

    /* 4b. Commit Directory Index Entry */
    if (found_in_root_attr) {
        /* Mutate root_mft buffer and re-encode */
        rcur = rattr_off;
        while (rcur + 8 <= rec_size) {
            uint32_t atype = get_le32(root_mft + rcur);
            uint32_t alen = get_le32(root_mft + rcur + 4);
            if (atype == 0xFFFFFFFF || alen == 0 || rcur + alen > rec_size) break;
            if (atype == 0x90 && root_mft[rcur + 8] == 0) {
                uint16_t voff = get_le16(root_mft + rcur + 20);
                uint32_t vlen = get_le32(root_mft + rcur + 16);
                index_rename(root_mft + rcur + voff + 16, root_mft + rcur + voff + vlen,
                             (uint64_t)target_inode, target_seq,
                             target_utf16, (uint8_t)target_len, /*commit=*/1, force_order);
                break;
            }
            rcur += alen;
        }
        ntfs_fixup_encode(root_mft, rec_size);
        if (io_write(fd, root_mft, rec_size, mft_base + 5ULL * rec_size) != 0) {
            perror("io_write root_mft");
            close(fd);
            return 1;
        }
        printf("[✓] Entrada de diretorio atualizada em $INDEX_ROOT (Inode 5)\n");
    } else if (found_in_alloc) {
        uint8_t *blk = malloc(idx_sz);
        if (!blk || io_read(fd, blk, idx_sz, found_alloc_blk_off) != 0 ||
            ntfs_fixup_decode(blk, idx_sz, "INDX") != 0) {
            fprintf(stderr, "[!] Erro ao reler/decodificar bloco INDX para commit\n");
            if (blk) free(blk);
            close(fd);
            return 1;
        }

        index_rename(blk + 0x18, blk + idx_sz,
                     (uint64_t)target_inode, target_seq,
                     target_utf16, (uint8_t)target_len, /*commit=*/1, force_order);

        ntfs_fixup_encode(blk, idx_sz);
        if (io_write(fd, blk, idx_sz, found_alloc_blk_off) != 0) {
            perror("io_write blk");
            free(blk);
            close(fd);
            return 1;
        }
        free(blk);
        printf("[✓] Entrada de diretorio atualizada em $INDEX_ALLOCATION (bloco em 0x%" PRIx64 ")\n", found_alloc_blk_off);
    }

    /* 5. Flush e sincronizacao eMMC garantida */
    if (fsync(fd) < 0) {
        perror("fsync");
        close(fd);
        return 1;
    }
    close(fd);

    printf("[✓] Alternacao concluida com sucesso e conformidade total NTFS: TV detectara '%s'\n", target_name);
    return 0;
}
