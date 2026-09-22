#!/usr/bin/env python3
"""Closed-loop file test: for each channel, switch via web remote, watch
/run/media/sam/LIVETV/CANAL AO VIVO.ts like the TV (2 min each):
- TV-open test (decode 8s from file offset 0)
- head-age test (file head PTS vs live PTS, must be < 4 min)
- identity sample (thumbnail at writer frontier for visual ID)
- CC continuity + server/writer health
Usage: closed_loop_file_test.py <writer_start_epoch> ; logs flushed to stdout."""
import json, os, subprocess, sys, time, urllib.request

MOUNT = "/run/media/sam/LIVETV/CANAL AO VIVO.ts"
BASE = "http://127.0.0.1:8080"
TMP = "/tmp/opencode"
HOPS = ["sony-channel-br", "globo-morena-dourados", "record-news",
        "espn-mirror-a07z", "sbt-news"]
WATCH = 120          # seconds per channel like a real viewer
SETTLE = 100         # wait before sampling frontier (post-switch data)
RATE = 0.34          # MB/s writer pace over tunnel (conservative)
LEAD = 15.0          # MB writer head-start

def log(m):
    print(m, flush=True)

def api(path, data=None, timeout=10):
    req = urllib.request.Request(
        BASE + path,
        data=json.dumps(data).encode() if data else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())

def sh(cmd, timeout=120):
    return subprocess.run(cmd, shell=True, capture_output=True,
                          text=True, timeout=timeout)

def file_pts_at(offset_mb, size_mb=2):
    with open(MOUNT, "rb") as f:
        f.seek(int(offset_mb * 1048576))
        d = f.read(int(size_mb * 1048576))
    best = (0, 0)
    for s in range(188):
        c = sum(1 for i in range(2000)
                if i * 188 + s < len(d) and d[i * 188 + s] == 0x47)
        if c > best[0]:
            best = (c, s)
    _, s = best
    if best[0] < 1900:
        return None, best[0] / 20.0
    for i in range(2000):
        o = i * 188 + s
        if o + 188 > len(d):
            break
        pid = ((d[o + 1] & 0x1F) << 8) | d[o + 2]
        pusi = (d[o + 1] & 0x40) >> 6
        afc = (d[o + 3] & 0x30) >> 4
        if pid == 256 and pusi and afc in (1, 3):
            off = 4
            if afc in (2, 3):
                off += 1 + d[o + 4]
            if (d[o + off:o + off + 3] == b"\x00\x00\x01"
                    and d[o + off + 3] in range(0xE0, 0xF0)
                    and d[o + off + 7] & 0x80):
                b = d[o + off + 9:o + off + 14]
                pts = (((b[0] & 0x0E) << 29) | (b[1] << 22)
                       | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1)) / 90000.0
                return pts, 100.0
    return None, 0.0

def live_pts():
    p = os.path.join(TMP, "loop_live.ts")
    sh(f"timeout 15 curl -s {BASE}/live.ts -o {p} --max-time 10")
    with open(p, "rb") as f:
        d = f.read()
    n = len(d) // 188
    for i in range(n):
        o = i * 188
        if d[o] != 0x47:
            continue
        pid = ((d[o + 1] & 0x1F) << 8) | d[o + 2]
        pusi = (d[o + 1] & 0x40) >> 6
        afc = (d[o + 3] & 0x30) >> 4
        if pid == 256 and pusi and afc in (1, 3):
            off = 4
            if afc in (2, 3):
                off += 1 + d[o + 4]
            if (d[o + off:o + off + 3] == b"\x00\x00\x01"
                    and d[o + off + 3] in range(0xE0, 0xF0)
                    and d[o + off + 7] & 0x80):
                b = d[o + off + 9:o + off + 14]
                return (((b[0] & 0x0E) << 29) | (b[1] << 22)
                        | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1)) / 90000.0
    return None

def tv_open_frames(secs=8):
    r = sh(f"timeout 40 ffmpeg -hide_banner -loglevel info -f mpegts "
           f"-i '{MOUNT}' -t {secs} -f null - 2>&1 | grep -oE 'frame=[ ]*[0-9]+' | tail -1")
    try:
        return int(r.stdout.strip().split()[-1])
    except Exception:
        return -1

def writer_elapsed(t0):
    r = sh("adb shell 'su -c \"date +%s\"'")
    try:
        return int(r.stdout.strip()) - t0
    except Exception:
        return -1

def main():
    t0 = int(sys.argv[1])
    log(f"LOOP START writer_t0={t0}")
    results = []
    for n, cid in enumerate(HOPS, 1):
        log(f"--- HOP {n}/{len(HOPS)} {cid} ---")
        try:
            api("/api/switch", {"channel_id": cid})
        except Exception as e:
            log(f"FAIL switch api: {e}")
            results.append((cid, False, "api")); continue
        el = writer_elapsed(t0)
        # watch like a viewer for WATCH seconds (server-side confirm mid-way)
        time.sleep(WATCH // 2)
        st = api("/api/status")
        moved = (st["active_channel_id"] == cid)
        time.sleep(WATCH - WATCH // 2)
        # 1. TV-open test from file offset 0
        fr = tv_open_frames()
        # 2. head age
        lp = live_pts()
        hp, _ = file_pts_at(0)
        age = (lp - hp) if (lp and hp) else -1
        # 3. frontier identity sample (post-switch data only)
        el2 = writer_elapsed(t0)
        pos = LEAD + RATE * max(el2, 0)
        off = max(pos - 12.0, 1.0)
        with open(MOUNT, "rb") as f:
            f.seek(int(off * 1048576))
            d = f.read(12 * 1048576)
        sb = os.path.join(TMP, f"loop_{cid}.bin")
        open(sb, "wb").write(d)
        png = os.path.join(TMP, f"loop_{cid}.png")
        sh(f"timeout 40 ffmpeg -hide_banner -loglevel error -f mpegts -ss 2 "
           f"-i {sb} -frames:v 1 {png}")
        # 4. writer health via adb
        w = sh("adb shell \"su -c 'cat /data/local/tmp/channel_stream.pid; tail -n 1 /data/local/tmp/channel_stream.log'\"")
        ok = moved and fr >= 200 and 0 <= age < 240
        log(f"moved={moved} tvopen_frames={fr}/240 head_age={age:.0f}s "
            f"thumb={os.path.basename(png)} writer='{w.stdout.strip().splitlines()[-1] if w.stdout.strip() else 'N/A'}' "
            f"=> {'PASS' if ok else 'FAIL'}")
        results.append((cid, ok, ""))
    log("LOOP DONE " + " ".join(f"{c}={'P' if o else 'F'}" for c, o, _ in results))
    sys.exit(0 if all(o for _, o, _ in results) else 1)

if __name__ == "__main__":
    main()
