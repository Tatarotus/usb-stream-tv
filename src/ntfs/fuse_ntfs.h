#ifndef FUSE_NTFS_H
#define FUSE_NTFS_H

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <pthread.h>

/* ---- Geometry constants for NTFS disks ---- */
#define BPS 512ULL
#define SPC 8ULL
#define CLUSTER_SIZE (BPS * SPC)                    /* 4096 bytes */
#define NTFS_FILE_SIZE 8000000000ULL                /* Default single-file: 8,000,000,000 bytes */
#define NTFS_TOTAL_SECTORS 16777216ULL              /* 8.0 GiB = 16,777,216 sectors */
#define NTFS_DISK_SIZE (NTFS_TOTAL_SECTORS * BPS)   /* 8,589,934,592 bytes */
#define RINGSZ (128ULL * 1024 * 1024)               /* 128 MiB live ring buffer */

#define MAX_EXTENTS_PER_FILE 8
#define MAX_VIRTUAL_FILES 32

/* Structure representing an NTFS data runlist extent */
struct ntfs_extent {
    uint32_t id;
    uint64_t vcn_start;      /* Starting VCN (clusters in file) */
    uint64_t vcn_end;        /* Ending VCN (inclusive) */
    uint64_t lcn_start;      /* Starting LCN (clusters on disk) */
    uint64_t lcn_end;        /* Ending LCN (inclusive) */
    uint64_t file_start;     /* Starting byte offset in virtual file */
    uint64_t file_end;       /* Ending byte offset in virtual file (exclusive) */
    uint64_t lba_start;      /* Starting LBA sector on disk */
    uint64_t lba_end;        /* Ending LBA sector on disk (inclusive) */
    uint64_t num_clusters;   /* Number of 4KiB clusters in this extent */
};

/* The 3 extents verified factually from the 8 GB NTFS prototype */
static const struct ntfs_extent NTFS_EXTENTS[3] = {
    {
        .id = 0,
        .vcn_start = 0ULL,
        .vcn_end = 907017ULL,
        .lcn_start = 1190133ULL,
        .lcn_end = 2097150ULL,
        .file_start = 0ULL,
        .file_end = 3715145728ULL,
        .lba_start = 9521064ULL,
        .lba_end = 16777207ULL,
        .num_clusters = 907018ULL
    },
    {
        .id = 1,
        .vcn_start = 907018ULL,
        .vcn_end = 1693280ULL,
        .lcn_start = 262312ULL,
        .lcn_end = 1048574ULL,
        .file_start = 3715145728ULL,
        .file_end = 6935678976ULL,
        .lba_start = 2098496ULL,
        .lba_end = 8388599ULL,
        .num_clusters = 786263ULL
    },
    {
        .id = 2,
        .vcn_start = 1693281ULL,
        .vcn_end = 1953124ULL,
        .lcn_start = 23ULL,
        .lcn_end = 259866ULL,
        .file_start = 6935678976ULL,
        .file_end = 8000000000ULL,
        .lba_start = 184ULL,
        .lba_end = 2078935ULL,
        .num_clusters = 259844ULL
    }
};

/* Multi-File VOD and Live Types */
enum source_type {
    SRC_LIVE_FIFO = 0,
    SRC_VOD_HTTP_RANGE = 1,
    SRC_TEST_PATTERN = 2
};

enum vod_state {
    VOD_STATE_PROCESSING = 0,
    VOD_STATE_READY = 1
};

struct virtual_file {
    int file_id;
    char id[32];
    char category[32];
    char path[128];
    char name[64];
    uint64_t file_size;
    enum source_type src_type;
    enum vod_state state;
    uint64_t bytes_available;
    int num_extents;
    struct ntfs_extent extents[MAX_EXTENTS_PER_FILE];
    char http_host[64];
    int http_port;
    char http_path[128];
    int active;
    int http_sock;
    pthread_mutex_t vf_mu;
    pthread_cond_t vf_cv;
    /* Telemetry tracking */
    uint64_t tel_count;
    uint64_t tel_last_t_ms;
    uint64_t tel_last_foff;
    size_t   tel_last_sz;
    uint64_t tel_max_foff;
};

/* Result codes for virtual_file_lookup */
#define VIRT_RES_METADATA -1
#define VIRT_RES_EOF      -2

/* Multi-file LBA lookup:
 * Returns file index (>= 0) if sector belongs to a virtual file,
 * VIRT_RES_EOF (-2) if past file EOF,
 * or VIRT_RES_METADATA (-1) if filesystem metadata.
 */
static inline int virtual_file_lookup(const struct virtual_file *files, int num_files,
                                      uint64_t lba, size_t sec_off,
                                      int *out_file_idx, uint64_t *out_foff,
                                      uint64_t *out_avail_in_ext)
{
    for (int fi = 0; fi < num_files; fi++) {
        const struct virtual_file *vf = &files[fi];
        if (!vf->active) continue;
        for (int ei = 0; ei < vf->num_extents; ei++) {
            const struct ntfs_extent *ext = &vf->extents[ei];
            if (lba >= ext->lba_start && lba <= ext->lba_end) {
                uint64_t sec_delta = lba - ext->lba_start;
                uint64_t foff = ext->file_start + sec_delta * BPS + (uint64_t)sec_off;
                if (foff >= vf->file_size) {
                    return VIRT_RES_EOF;
                }
                if (out_file_idx) *out_file_idx = fi;
                if (out_foff) *out_foff = foff;
                if (out_avail_in_ext) *out_avail_in_ext = ext->file_end - foff;
                return fi;
            }
        }
    }
    return VIRT_RES_METADATA;
}

/* Maps virtual file offset (foff) to NTFS VCN, LCN, LBA, and sector offset.
 * Returns extent index (0, 1, 2) on success, or -1 on EOF/error. */
static inline int ntfs_foff_to_lba(uint64_t foff,
                                   uint64_t *out_vcn,
                                   uint64_t *out_lcn,
                                   uint64_t *out_lba,
                                   size_t *out_sec_off,
                                   uint64_t *out_avail_in_extent)
{
    if (foff >= NTFS_FILE_SIZE) {
        return -1; /* EOF reached */
    }
    for (int i = 0; i < 3; i++) {
        if (foff >= NTFS_EXTENTS[i].file_start && foff < NTFS_EXTENTS[i].file_end) {
            uint64_t delta_bytes = foff - NTFS_EXTENTS[i].file_start;
            uint64_t delta_clusters = delta_bytes / CLUSTER_SIZE;
            if (out_vcn) *out_vcn = NTFS_EXTENTS[i].vcn_start + delta_clusters;
            if (out_lcn) *out_lcn = NTFS_EXTENTS[i].lcn_start + delta_clusters;
            if (out_lba) *out_lba = NTFS_EXTENTS[i].lba_start + (delta_bytes / BPS);
            if (out_sec_off) *out_sec_off = (size_t)(delta_bytes % BPS);
            if (out_avail_in_extent) *out_avail_in_extent = NTFS_EXTENTS[i].file_end - foff;
            return i;
        }
    }
    return -2;
}

/* Maps an LBA sector and byte offset within sector back to virtual file offset (foff).
 * Returns extent index (0, 1, 2) if the sector belongs to TV AO VIVO.ts,
 * or -1 if the sector is a filesystem metadata sector. */
static inline int ntfs_lba_to_foff(uint64_t lba,
                                   size_t sec_off,
                                   uint64_t *out_foff,
                                   uint64_t *out_avail_in_extent)
{
    for (int i = 0; i < 3; i++) {
        if (lba >= NTFS_EXTENTS[i].lba_start && lba <= NTFS_EXTENTS[i].lba_end) {
            uint64_t sec_delta = lba - NTFS_EXTENTS[i].lba_start;
            uint64_t foff = NTFS_EXTENTS[i].file_start + sec_delta * BPS + (uint64_t)sec_off;
            if (foff >= NTFS_FILE_SIZE) {
                return -1;
            }
            if (out_foff) *out_foff = foff;
            if (out_avail_in_extent) *out_avail_in_extent = NTFS_EXTENTS[i].file_end - foff;
            return i;
        }
    }
    return -1; /* Metadata sector (Boot, MFT, Bitmap, LogFile, etc.) */
}

/* Helper to map file offset to live ring buffer position */
static inline uint64_t ntfs_foff_to_ring_pos(uint64_t base, uint64_t foff, size_t *out_ring_idx)
{
    uint64_t abs_pos = base + foff;
    if (out_ring_idx) {
        *out_ring_idx = (size_t)(abs_pos % RINGSZ);
    }
    return abs_pos;
}

#endif /* FUSE_NTFS_H */
