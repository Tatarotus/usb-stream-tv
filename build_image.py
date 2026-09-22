#!/usr/bin/env python3
"""
build_image.py — Creates a FAT32 disk image for USB Stream TV
The image contains a single file 'CANAL AO VIVO.ts' sized to exactly
60 MiB (62914560 bytes) pre-filled with null TS packets.

The layout matches the original tv_stream_single.img:
  - Cluster 3 (file data) starts at sector 3112 (offset 1593344)
  - FAT size = 1536 sectors (matching the old image)
  - This keeps stream_writer.py's sector 3112 constant working correctly
"""
import struct
import os
import sys

img_file = "/home/sam/Code/usb-stream-tv/tv_stream_fixed.img"

# Must match old image layout so sector 3112 = cluster 3 data start
bytes_per_sector = 512
sectors_per_cluster = 8
cluster_size = bytes_per_sector * sectors_per_cluster  # 4096
reserved_sectors = 32
num_fats = 2
sectors_per_fat = 1536  # Match the OLD image — critical for sector 3112
root_dir_cluster = 2
file_start_cluster = 3
media_descriptor = 0xF8

file_size = 62758912  # 59.85 MiB = 326 * 192512 = LCM(188, 4096) multiple:
# exactly 333824 TS packets AND 15322 FAT32 clusters (no torn tail packet
# at the file end; 62914560 % 188 was 172 stray bytes before).
num_file_clusters = file_size // cluster_size  # 15322

# Calculate data region start
data_region_sector = reserved_sectors + num_fats * sectors_per_fat  # 32 + 2*1536 = 3104
# Cluster 2 (root dir) is at sector 3104
# Cluster 3 (file data) is at sector 3104 + 8 = 3112  ← matches the hardcoded value!
cluster_3_sector = data_region_sector + sectors_per_cluster  # 3112
cluster_3_offset = cluster_3_sector * bytes_per_sector  # 1593344

# Total image size: FAT32 requires >= 65525 clusters to be spec-legal
# (some TV firmwares reject smaller FAT32 volumes).
# 300 MiB / 4 KiB = 76800 clusters. File stays 60 MiB at cluster 3.
total_sectors = (300 * 1024 * 1024) // bytes_per_sector  # 614400
total_bytes = total_sectors * bytes_per_sector
total_clusters = total_sectors // sectors_per_cluster  # 76800

print(f"=== Image Layout ===")
print(f"Data region: sector {data_region_sector} (offset {data_region_sector * 512})")
print(f"Root dir (cluster 2): sector {data_region_sector}")
print(f"File data (cluster 3): sector {cluster_3_sector} (offset {cluster_3_offset})")
print(f"File size: {file_size} bytes ({file_size/1024/1024:.0f} MiB)")
print(f"File clusters: {num_file_clusters} (clusters 3..{file_start_cluster + num_file_clusters - 1})")
print(f"Total image: {total_sectors} sectors ({total_bytes/1024/1024:.1f} MiB)")

with open(img_file, "wb") as f:
    # Pre-allocate the entire image with zeros
    f.seek(total_bytes - 1)
    f.write(b'\x00')
    f.seek(0)

with open(img_file, "r+b") as f:
    # ====== 1. Boot Sector (sector 0) ======
    boot = bytearray(512)
    boot[0:3] = b'\xEB\x58\x90'          # Jump instruction
    boot[3:11] = b'mkfs.fat'             # OEM ID
    struct.pack_into('<H', boot, 11, bytes_per_sector)
    boot[13] = sectors_per_cluster
    struct.pack_into('<H', boot, 14, reserved_sectors)
    boot[16] = num_fats
    struct.pack_into('<H', boot, 17, 0)   # Root entry count (0 for FAT32)
    struct.pack_into('<H', boot, 19, 0)   # Total sectors 16 (0 for FAT32)
    boot[21] = media_descriptor
    struct.pack_into('<H', boot, 22, 0)   # FAT size 16 (0 for FAT32)
    struct.pack_into('<H', boot, 24, 63)  # Sectors per track
    struct.pack_into('<H', boot, 26, 255) # Number of heads
    struct.pack_into('<I', boot, 28, 0)   # Hidden sectors
    struct.pack_into('<I', boot, 32, total_sectors)
    # FAT32 specific
    struct.pack_into('<I', boot, 36, sectors_per_fat)
    struct.pack_into('<H', boot, 40, 0)   # Ext flags
    struct.pack_into('<H', boot, 42, 0)   # FS version
    struct.pack_into('<I', boot, 44, root_dir_cluster)
    struct.pack_into('<H', boot, 48, 1)   # FSInfo sector
    struct.pack_into('<H', boot, 50, 6)   # Backup boot sector
    # bytes 52-63: reserved
    boot[64] = 0x80   # Drive number
    boot[65] = 0      # Reserved
    boot[66] = 0x29   # Extended boot signature
    struct.pack_into('<I', boot, 67, 0x54563031)  # Volume serial ('TV01' - fresh serial purges Samsung NVRAM cache)
    boot[71:82] = b'LIVETV     '          # Volume label
    boot[82:90] = b'FAT32   '            # FS type
    boot[510] = 0x55
    boot[511] = 0xAA
    f.write(boot)

    # ====== 2. FSInfo Sector (sector 1) ======
    f.seek(1 * bytes_per_sector)
    fsinfo = bytearray(512)
    fsinfo[0:4] = b'RRaA'      # Lead signature
    fsinfo[484:488] = b'rrAa'  # Struct signature
    used_clusters = 2 + 1 + num_file_clusters  # reserved 0,1 + root + file
    free_clusters = total_clusters - used_clusters
    struct.pack_into('<I', fsinfo, 488, max(0, free_clusters))
    struct.pack_into('<I', fsinfo, 492, file_start_cluster + num_file_clusters)
    fsinfo[508:512] = b'\x00\x00\x55\xAA'
    f.write(fsinfo)

    # ====== 3. Backup Boot Sector (sector 6) ======
    f.seek(0)
    boot_data = f.read(512)
    f.seek(6 * bytes_per_sector)
    f.write(boot_data)

    # ====== 4. FAT Tables ======
    fat = bytearray(sectors_per_fat * bytes_per_sector)
    
    # Cluster 0: media descriptor
    struct.pack_into('<I', fat, 0, 0x0FFFFFF8)
    # Cluster 1: end of chain marker
    struct.pack_into('<I', fat, 4, 0x0FFFFFFF)
    # Cluster 2: root directory (end of chain — single cluster)
    struct.pack_into('<I', fat, 8, 0x0FFFFFFF)
    # Clusters 3..(3+num_file_clusters-2): sequential chain
    for c in range(file_start_cluster, file_start_cluster + num_file_clusters - 1):
        struct.pack_into('<I', fat, c * 4, c + 1)
    # Last cluster: end of chain
    struct.pack_into('<I', fat, (file_start_cluster + num_file_clusters - 1) * 4, 0x0FFFFFFF)

    # Write both FAT copies
    f.seek(reserved_sectors * bytes_per_sector)
    f.write(fat)
    f.write(fat)

    # ====== 5. Root Directory (cluster 2 at sector 3104) ======
    root_dir_offset = data_region_sector * bytes_per_sector  # sector 3104
    f.seek(root_dir_offset)

    # Volume label entry
    vol_entry = bytearray(32)
    vol_entry[0:11] = b'LIVETV     '
    vol_entry[11] = 0x08  # Volume label attribute
    f.write(vol_entry)

    # Long File Name entries for "TV AO VIVO.ts"
    # Short name for checksum
    short_name = b'TVAOVI~1TS '  # 8.3 format: "TVAOVI~1" + "TS "
    chksum = 0
    for byte in short_name:
        chksum = (((chksum & 1) << 7) | ((chksum & 0xfe) >> 1)) + byte
        chksum &= 0xff

    lfn_name = "TV AO VIVO.ts"
    # Pad to multiple of 13 chars
    lfn_chars = list(lfn_name)
    lfn_chars.append('\x00')  # Null terminator
    while len(lfn_chars) % 13 != 0:
        lfn_chars.append('\xff')  # Padding
    
    num_lfn = len(lfn_chars) // 13  # 2 entries

    # Write LFN entries in REVERSE order
    for seq in range(num_lfn, 0, -1):
        entry = bytearray(32)
        seq_byte = seq
        if seq == num_lfn:
            seq_byte |= 0x40  # Last LFN entry flag
        entry[0] = seq_byte
        
        chars = lfn_chars[(seq-1)*13 : seq*13]
        char_bytes = ''.join(chars).encode('utf-16le')
        
        # LFN name spread across three fields
        entry[1:11] = char_bytes[0:10]    # chars 1-5
        entry[11] = 0x0F                   # LFN attribute
        entry[12] = 0x00                   # Type
        entry[13] = chksum
        entry[14:26] = char_bytes[10:22]  # chars 6-11
        entry[26:28] = b'\x00\x00'        # Cluster (always 0 for LFN)
        entry[28:32] = char_bytes[22:26]  # chars 12-13
        f.write(entry)

    # Short name directory entry
    sfn_entry = bytearray(32)
    sfn_entry[0:11] = short_name
    sfn_entry[11] = 0x20      # Archive attribute
    sfn_entry[12] = 0x00      # NT reserved
    sfn_entry[13] = 0x00      # Created time (tenths)
    # Created time/date
    struct.pack_into('<H', sfn_entry, 14, 0x6000)  # Time
    struct.pack_into('<H', sfn_entry, 16, 0x524F)  # Date
    struct.pack_into('<H', sfn_entry, 18, 0x524F)  # Access date
    # First cluster high word
    struct.pack_into('<H', sfn_entry, 20, (file_start_cluster >> 16) & 0xFFFF)
    # Modified time/date
    struct.pack_into('<H', sfn_entry, 22, 0x6000)  # Time
    struct.pack_into('<H', sfn_entry, 24, 0x524F)  # Date
    # First cluster low word
    struct.pack_into('<H', sfn_entry, 26, file_start_cluster & 0xFFFF)
    # File size
    struct.pack_into('<I', sfn_entry, 28, file_size)
    f.write(sfn_entry)

    # ====== 6. Pre-fill File Data with Null TS Packets ======
    print(f"Pre-filling {file_size/1024/1024:.0f} MiB with null TS packets...")
    f.seek(cluster_3_offset)
    
    # Null TS packet: sync byte 0x47, PID 0x1FFF (null), no adaptation, payload of zeros
    null_pkt = bytearray(188)
    null_pkt[0] = 0x47
    null_pkt[1] = 0x1F
    null_pkt[2] = 0xFF
    null_pkt[3] = 0x10  # has payload, no adaptation field
    null_pkt = bytes(null_pkt)
    
    # Write in large chunks for speed
    chunk = null_pkt * 10000  # ~1.88 MB per write
    written = 0
    while written < file_size:
        to_write = min(len(chunk), file_size - written)
        f.write(chunk[:to_write])
        written += to_write

    f.flush()

print(f"\n✅ Image created: {img_file}")
print(f"   Size: {os.path.getsize(img_file)} bytes ({os.path.getsize(img_file)/1024/1024:.1f} MiB)")
print(f"   File 'CANAL AO VIVO.ts' = {file_size} bytes ({file_size/1024/1024:.0f} MiB)")
print(f"   Data starts at sector 3112 (offset {cluster_3_offset})")
