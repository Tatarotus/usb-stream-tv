#!/usr/bin/env python3
"""Prefill CANAL AO VIVO.ts head (first 16MB) with current live channel,
so the TV starts on fresh data instead of a previous run."""
import sys, urllib.request

IMG = "/data/local/tmp/tv_stream.img"
START_SECTOR = int(sys.argv[1]) if len(sys.argv) > 1 else 3112
SERVER = sys.argv[2] if len(sys.argv) > 2 else "https://academy-sometimes-stands-muscles.trycloudflare.com"
NEED = 16 * 1048576

off = START_SECTOR * 512
got = 0
f = open(IMG, "r+b", buffering=0)
f.seek(off)
req = urllib.request.Request(SERVER + "/live.ts", headers={"User-Agent": "USBStreamTV/2.0"})
with urllib.request.urlopen(req, timeout=20) as r:
    while got < NEED:
        c = r.read(65424)
        if not c:
            break
        f.write(c)
        got += len(c)
f.flush()
print(f"prefill done: {got/1048576:.1f} MB at offset {off}")
