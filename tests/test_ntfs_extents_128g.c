#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <inttypes.h>
#include "../src/ntfs/fuse_ntfs.h"

int main(void) {
    printf("=== Testing 128 GiB NTFS Extents Bidirectional Translation ===\n");

    /* 1. Verify Extent Boundaries */
    assert(NTFS_NUM_EXTENTS == 3);
    assert(NTFS_FILE_SIZE == 128000000000ULL);
    assert(NTFS_TOTAL_SECTORS == 268435456ULL);

    uint64_t total_clusters = 0;
    for (int i = 0; i < NTFS_NUM_EXTENTS; i++) {
        const struct ntfs_extent *e = &NTFS_EXTENTS[i];
        assert(e->vcn_end - e->vcn_start + 1 == e->num_clusters);
        assert(e->lcn_end - e->lcn_start + 1 == e->num_clusters);
        assert(e->file_end - e->file_start == e->num_clusters * CLUSTER_SIZE);
        assert((e->lba_end - e->lba_start + 1) == e->num_clusters * SPC);
        assert(e->lba_start == e->lcn_start * SPC);
        total_clusters += e->num_clusters;
    }
    assert(total_clusters == 31250000ULL);
    assert(total_clusters * CLUSTER_SIZE == NTFS_FILE_SIZE);
    printf("  [PASS] All 3 extent boundaries and cluster counts strictly consistent.\n");

    /* 2. Test foff_to_lba and lba_to_foff roundtrips */
    uint64_t test_foffs[] = {
        0ULL,
        512ULL,
        4096ULL,
        1000000ULL,
        NTFS_EXTENTS[0].file_end - 512,  /* end of extent 0 */
        NTFS_EXTENTS[1].file_start,      /* start of extent 1 */
        25000000000ULL,
        NTFS_EXTENTS[1].file_end - 512,  /* end of extent 1 */
        NTFS_EXTENTS[2].file_start,      /* start of extent 2 */
        80000000000ULL,
        NTFS_EXTENTS[2].file_end - 512,  /* last sector of file */
    };

    size_t num_tests = sizeof(test_foffs) / sizeof(test_foffs[0]);
    for (size_t t = 0; t < num_tests; t++) {
        uint64_t foff = test_foffs[t];
        uint64_t vcn = 0, lcn = 0, lba = 0, avail = 0;
        size_t sec_off = 0;

        int ext_idx = ntfs_foff_to_lba(foff, &vcn, &lcn, &lba, &sec_off, &avail);
        assert(ext_idx >= 0 && ext_idx < NTFS_NUM_EXTENTS);

        uint64_t back_foff = 0, back_avail = 0;
        int back_ext = ntfs_lba_to_foff(lba, sec_off, &back_foff, &back_avail);
        assert(back_ext == ext_idx);
        assert(back_foff == foff);
        assert(back_avail == avail);
    }
    printf("  [PASS] Roundtrip foff <-> lba verified across extent transition points.\n");

    /* 3. Test EOF and Out-of-bounds */
    uint64_t eof_foff = NTFS_FILE_SIZE;
    uint64_t vcn = 0, lcn = 0, lba = 0, avail = 0;
    size_t sec_off = 0;
    int res = ntfs_foff_to_lba(eof_foff, &vcn, &lcn, &lba, &sec_off, &avail);
    assert(res == -1);

    res = ntfs_foff_to_lba(eof_foff + 1000000ULL, &vcn, &lcn, &lba, &sec_off, &avail);
    assert(res == -1);
    printf("  [PASS] EOF detection strictly clamped at 128,000,000,000 bytes.\n");

    /* 4. Test Metadata LBAs return -1 (not mapped to virtual file) */
    uint64_t meta_lbas[] = {
        0,      /* Boot sector */
        32,     /* $MFT start (LCN 4 * 8) */
        33554432, /* Root dir & $Bitmap start (LCN 4194304 * 8) */
        134217720, /* $MFTMirr (LCN 16777215 * 8) */
        268435455 /* Backup Boot Sector */
    };
    for (size_t m = 0; m < sizeof(meta_lbas) / sizeof(meta_lbas[0]); m++) {
        uint64_t back_foff = 0, back_avail = 0;
        int back_ext = ntfs_lba_to_foff(meta_lbas[m], 0, &back_foff, &back_avail);
        assert(back_ext == -1);
    }
    printf("  [PASS] Filesystem metadata LBAs correctly identified and protected.\n");

    printf("\n=== ALL EXTENT UNIT TESTS PASSED (100%%) ===\n");
    return 0;
}
