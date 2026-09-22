#!/usr/bin/env python3
"""gen_template.py — builds fat_template.bin for fuse_direct.
Contains ONLY static metadata sectors (boot, FSInfo, root dir); all FAT
sectors and file data are synthesized/served live by the daemon.
Geometry MUST match fuse_direct.c constants:
  reserved=32, fats=2, spf=8192, root cluster 2, file cluster 3, spc=8.
File: 'TV AO VIVO.ts', size 0xFFFFFFFF (4294967295).
"""
import struct

BPS, SPC, RSV, FATS, SPF = 512, 8, 32, 2, 8192
DATA_SEC = RSV + FATS * SPF          # 16416: cluster 2 (root dir)
FILE_SEC = DATA_SEC + SPC            # 16424: cluster 3 (file data)
FILE_SIZE = 1800000000               # 1.8 GB (~96 min @ 305KB/s, completely safe for signed 32-bit FAT32)
NFILECLUS = (FILE_SIZE + SPC * BPS - 1) // (SPC * BPS)
TOTAL_CLUS = 1048576                 # clusters 0..1048575
TOTAL_SEC = DATA_SEC + (TOTAL_CLUS - 2) * SPC
FREE_CLUS = (TOTAL_CLUS - 2) - 1 - NFILECLUS
SHORT = b'TVAOVI~1TS '

out = {}

boot = bytearray(512)
boot[0:3] = b'\xEB\x58\x90'
boot[3:11] = b'mkfs.fat'
struct.pack_into('<H', boot, 11, BPS)
boot[13] = SPC
struct.pack_into('<H', boot, 14, RSV)
boot[16] = FATS
struct.pack_into('<H', boot, 17, 0)
struct.pack_into('<H', boot, 19, 0)
boot[21] = 0xF8
struct.pack_into('<H', boot, 22, 0)
struct.pack_into('<H', boot, 24, 63)
struct.pack_into('<H', boot, 26, 255)
struct.pack_into('<I', boot, 28, 0)
struct.pack_into('<I', boot, 32, TOTAL_SEC)
struct.pack_into('<I', boot, 36, SPF)
struct.pack_into('<H', boot, 40, 0)
struct.pack_into('<H', boot, 42, 0)
struct.pack_into('<I', boot, 44, 2)
struct.pack_into('<H', boot, 48, 1)
struct.pack_into('<H', boot, 50, 6)
boot[64] = 0x80
boot[66] = 0x29
struct.pack_into('<I', boot, 67, 0x12345678)
boot[71:82] = b'LIVETV     '
boot[82:90] = b'FAT32   '
boot[510] = 0x55
boot[511] = 0xAA
out[0] = bytes(boot)

fsinfo = bytearray(512)
fsinfo[0:4] = b'RRaA'
fsinfo[484:488] = b'rrAa'
struct.pack_into('<I', fsinfo, 488, FREE_CLUS)
struct.pack_into('<I', fsinfo, 492, 3 + NFILECLUS)
fsinfo[508:512] = b'\x00\x00\x55\xAA'
out[1] = bytes(fsinfo)
out[6] = bytes(boot)  # backup boot

root = bytearray(SPC * BPS)
vol = bytearray(32)
vol[0:11] = b'LIVETV     '
vol[11] = 0x08
root[0:32] = vol

chksum = 0
for byte in SHORT:
    chksum = (((chksum & 1) << 7) | ((chksum & 0xfe) >> 1)) + byte
    chksum &= 0xff
lfn = list("TV AO VIVO.ts") + ['\x00']
while len(lfn) % 13:
    lfn.append('\xff')
n_lfn = len(lfn) // 13
off = 32
for seq in range(n_lfn, 0, -1):
    e = bytearray(32)
    sb = seq | (0x40 if seq == n_lfn else 0)
    e[0] = sb
    cb = ''.join(lfn[(seq - 1) * 13:seq * 13]).encode('utf-16le')
    e[1:11] = cb[0:10]
    e[11] = 0x0F
    e[12] = 0x00
    e[13] = chksum
    e[14:26] = cb[10:22]
    e[26:28] = b'\x00\x00'
    e[28:32] = cb[22:26]
    root[off:off + 32] = e
    off += 32

sfn = bytearray(32)
sfn[0:11] = SHORT
sfn[11] = 0x20
struct.pack_into('<H', sfn, 14, 0x6000)
struct.pack_into('<H', sfn, 16, 0x524F)
struct.pack_into('<H', sfn, 18, 0x524F)
struct.pack_into('<H', sfn, 20, 0)
struct.pack_into('<H', sfn, 22, 0x6000)
struct.pack_into('<H', sfn, 24, 0x524F)
struct.pack_into('<H', sfn, 26, 3)
struct.pack_into('<I', sfn, 28, FILE_SIZE)
root[off:off + 32] = sfn
for i in range(DATA_SEC, DATA_SEC + SPC):
    out[i] = bytes(root[(i - DATA_SEC) * BPS:(i - DATA_SEC + 1) * BPS])

with open("fat_template.bin", "wb") as f:
    for sec in sorted(out):
        assert len(out[sec]) == BPS, sec
        f.write(struct.pack('<I', sec) + out[sec])
print(f"template: {len(out)} sectors, root@sec {DATA_SEC}, file@sec {FILE_SEC}, "
      f"total_sec {TOTAL_SEC}, file_size {FILE_SIZE}")
