#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

int main(int argc, char *argv[]) {
    const char *path = (argc > 1 && argv[1][0] != '-') ? argv[1] : "/data/local/tmp/ntfs_lab/ntfs_template.bin";
    const char *force_mode = NULL;
    if (argc > 1 && argv[1][0] == '-') force_mode = argv[1];
    if (argc > 2 && argv[2][0] == '-') force_mode = argv[2];

    int fd = open(path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    /* Target names to alternate between */
    const char *name_a = "TV AO VIVO.trp";
    const char *name_b = "TV AO VIVO 2.tp";

    /* Read MFT Record 27 at 43 * 1024 */
    uint8_t mft[1024];
    if (pread(fd, mft, sizeof(mft), 43ULL * 1024) != sizeof(mft)) {
        perror("pread mft");
        close(fd);
        return 1;
    }

    /* Check if current is name_a or name_b */
    const char *target_name = name_b;
    uint8_t name_b_utf16[30];
    for (size_t i = 0; i < strlen(name_b); i++) {
        name_b_utf16[i * 2] = (uint8_t)name_b[i];
        name_b_utf16[i * 2 + 1] = 0;
    }

    /* Search for name_b in MFT */
    int found_b = 0;
    for (size_t i = 0; i + sizeof(name_b_utf16) <= sizeof(mft); i++) {
        if (memcmp(mft + i, name_b_utf16, sizeof(name_b_utf16)) == 0) {
            found_b = 1;
            break;
        }
    }

    if (force_mode && strcmp(force_mode, "--tp") == 0) {
        target_name = name_b;
    } else if (force_mode && strcmp(force_mode, "--trp") == 0) {
        target_name = name_a;
    } else {
        if (found_b) {
            target_name = name_a;
        } else {
            target_name = name_b;
        }
    }

    printf("[*] Alternating TV file: target is '%s'\n", target_name);

    size_t target_len = strlen(target_name);
    uint8_t target_utf16[32];
    for (size_t i = 0; i < target_len; i++) {
        target_utf16[i * 2] = (uint8_t)target_name[i];
        target_utf16[i * 2 + 1] = 0;
    }
    size_t target_bytes = target_len * 2;
    uint16_t val_len = (uint16_t)(66 + target_bytes);

    /* 1. Find $FILE_NAME attribute (type 0x30) in MFT record 27 */
    int fn_attr_off = -1;
    for (size_t i = 120; i + 90 < sizeof(mft); i += 8) {
        if (mft[i] == 0x30 && mft[i+1] == 0 && mft[i+2] == 0 && mft[i+3] == 0) {
            fn_attr_off = (int)i;
            break;
        }
    }

    if (fn_attr_off < 0) {
        fprintf(stderr, "[!] $FILE_NAME attribute not found in MFT record 27\n");
        close(fd);
        return 1;
    }

    /* Update Value length (resident attribute header byte 16) */
    mft[fn_attr_off + 16] = (uint8_t)(val_len & 0xFF);
    mft[fn_attr_off + 17] = (uint8_t)((val_len >> 8) & 0xFF);

    /* Filename info starts at fn_attr_off + 24 */
    int info_off = fn_attr_off + 24;
    mft[info_off + 64] = (uint8_t)target_len; /* Name length in characters */
    int name_off = info_off + 66;

    memcpy(mft + name_off, target_utf16, target_bytes);
    /* Zero padding up to 120 bytes of attribute */
    int pad_start = name_off + (int)target_bytes;
    int pad_end = fn_attr_off + 120;
    if (pad_end > pad_start) {
        memset(mft + pad_start, 0, (size_t)(pad_end - pad_start));
    }

    if (pwrite(fd, mft, sizeof(mft), 43ULL * 1024) != sizeof(mft)) {
        perror("pwrite mft");
        close(fd);
        return 1;
    }
    printf("[✓] Inode 27 patched to '%s'\n", target_name);

    /* 2. Directory Index at 1073763626 - 128 */
    uint8_t dir[256];
    uint64_t dir_off = 1073763626ULL - 128ULL;
    if (pread(fd, dir, sizeof(dir), dir_off) != sizeof(dir)) {
        perror("pread dir");
        close(fd);
        return 1;
    }

    /* Find Inode 27 file reference: 1b 00 00 00 00 00 02 00 */
    int entry_off = -1;
    for (size_t i = 0; i + 16 < sizeof(dir); i++) {
        if (dir[i] == 0x1b && dir[i+1] == 0 && dir[i+2] == 0 && dir[i+3] == 0 &&
            dir[i+4] == 0 && dir[i+5] == 0 && dir[i+6] == 0x02 && dir[i+7] == 0) {
            entry_off = (int)i;
            break;
        }
    }

    if (entry_off < 0) {
        fprintf(stderr, "[!] Directory entry for Inode 27 not found\n");
        close(fd);
        return 1;
    }

    /* Content length is at entry_off + 10 */
    dir[entry_off + 10] = (uint8_t)(val_len & 0xFF);
    dir[entry_off + 11] = (uint8_t)((val_len >> 8) & 0xFF);

    /* Name length is at entry_off + 16 + 64 */
    dir[entry_off + 80] = (uint8_t)target_len;
    int dir_name_off = entry_off + 82;
    memcpy(dir + dir_name_off, target_utf16, target_bytes);
    int dir_pad_start = dir_name_off + (int)target_bytes;
    int dir_pad_end = entry_off + 112;
    if (dir_pad_end > dir_pad_start) {
        memset(dir + dir_pad_start, 0, (size_t)(dir_pad_end - dir_pad_start));
    }

    if (pwrite(fd, dir, sizeof(dir), dir_off) != sizeof(dir)) {
        perror("pwrite dir");
        close(fd);
        return 1;
    }
    printf("[✓] Directory index patched to '%s'\n", target_name);

    close(fd);
    printf("[✓] Alternation complete: TV will see '%s'\n", target_name);
    return 0;
}
