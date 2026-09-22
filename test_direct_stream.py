#!/usr/bin/env python3
"""
test_direct_stream.py — Real-time Hardware USB Stream Reader for PC testing.
Bypasses the Linux kernel Host RAM Page Cache using O_DIRECT.
Reads continuously from the USB Mass Storage device and paces at exact 1x
playback rate (305 KB/s), reproducing how a television decoder hardware
consumes sectors from a circular buffer without caching.
"""

import os
import sys
import mmap
import time

FILE_PATH = "/run/media/sam/LIVETV/TV AO VIVO.ts"
if not os.path.exists(FILE_PATH) and os.path.exists("/run/media/sam/LIVETV/CANAL AO VIVO.ts"):
    FILE_PATH = "/run/media/sam/LIVETV/CANAL AO VIVO.ts"
FILE_SIZE = 62758912   # Exact packet-aligned size: 333824 * 188
BLOCK_SIZE = 192512    # LCM(188, 4096): 1024 TS packets, 47 FAT32 clusters
BYTES_PER_SEC = 305000 # 2.44 Mbps (exact matching rate of the stream)

def main():
    target = FILE_PATH
    if not os.path.exists(target):
        # Auto-detect any .ts file in mount dir
        if os.path.isdir("/run/media/sam/LIVETV"):
            for f in os.listdir("/run/media/sam/LIVETV"):
                if f.endswith(".ts"):
                    target = os.path.join("/run/media/sam/LIVETV", f)
                    break
    if not os.path.exists(target):
        sys.stderr.write(f"[-] Target file not found: {target}\n")
        sys.exit(1)

    sys.stderr.write("="*65 + "\n")
    sys.stderr.write("[*] Real-Time Direct USB Streamer Active (O_DIRECT)\n")
    sys.stderr.write("[*] Pacing: 1x real-time rate (305 KB/s)\n")
    sys.stderr.write(f"[*] Target: {FILE_PATH} ({FILE_SIZE} bytes = ~3m26s lap)\n")
    sys.stderr.write("="*65 + "\n")

    try:
        fd = os.open(target, os.O_RDONLY | os.O_DIRECT)
    except Exception as e:
        sys.stderr.write(f"[-] Failed to open with O_DIRECT: {e}\n")
        sys.exit(1)

    mm = mmap.mmap(-1, BLOCK_SIZE)
    offset = 0
    lap = 0
    total_streamed = 0
    start_time = time.time()

    try:
        while True:
            if offset + BLOCK_SIZE > FILE_SIZE:
                # Wrap directly on the hardware storage
                offset = 0
                lap += 1
                os.lseek(fd, 0, os.SEEK_SET)
                elapsed = time.time() - start_time
                sys.stderr.write(f"\n[✓ HARDWARE WRAP #{lap}] at T+{elapsed:.1f}s ({elapsed/60:.1f} min) — Reading fresh sectors from byte 0\n")

            n = os.readv(fd, [mm])
            if n <= 0:
                offset = 0
                os.lseek(fd, 0, os.SEEK_SET)
                time.sleep(0.01)
                continue

            # Stream direct TS packets to stdout
            try:
                sys.stdout.buffer.write(mm[:n])
                sys.stdout.buffer.flush()
            except (BrokenPipeError, IOError):
                break

            total_streamed += n
            offset += n

            # Pace at 1x real-time rate so reader never outruns the live stream
            target_elapsed = total_streamed / BYTES_PER_SEC
            now_elapsed = time.time() - start_time
            sleep_time = target_elapsed - now_elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        sys.stderr.write("\n[*] Streamer stopped by user.\n")
    finally:
        os.close(fd)

if __name__ == "__main__":
    main()
