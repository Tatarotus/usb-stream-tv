#!/usr/bin/env python3
"""Round-trip identity loop on the real mounted file.
Hops through channels and BACK, 2 min viewing each:
- switch via web remote, record writer pos at switch (heartbeat)
- watch 120s like a viewer
- drop host page cache (else USB reads look stale)
- TV-open decode 8s from offset 0 / head-age PTS check (<4min)
- frontier thumbnail from guaranteed post-switch region
- writer/server health
Usage: identity_loop.py ; logs flushed."""
import json, os, subprocess, sys, time, urllib.request

MOUNT = "/run/media/sam/LIVETV/CANAL AO VIVO.ts"
BASE = "http://127.0.0.1:8080"
TMP = "/tmp/opencode"
HOPS = ["globo-morena-dourados", "record-news", "sony-channel-br",
        "espn-mirror-a07z", "sbt-news", "sony-channel-br",
        "record-news", "globo-morena-dourados"]
WATCH = 120
WIN = 60 * 1024 * 1024  # writer window, must match stream_writer.py

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

def wpos():
    """(offset_bytes, total_bytes, channel, stamp) from phone heartbeat."""
    r = sh("adb shell \"su -c 'cat /data/local/tmp/writer_pos.txt'\"")
    try:
        p = r.stdout.strip().split()
        return int(p[0]), int(p[1]), p[2], p[3]
    except Exception:
        return None

def drop_caches():
    sh("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null")

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
        return None
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
                return (((b[0] & 0x0E) << 29) | (b[1] << 22)
                        | ((b[2] & 0xFE) << 14) | (b[3] << 7) | (b[4] >> 1)) / 90000.0
    return None

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

def main():
    drop_caches()
    results = []
    for n, cid in enumerate(HOPS, 1):
        log(f"--- HOP {n}/{len(HOPS)} {cid} ---")
        try:
            api("/api/switch", {"channel_id": cid})
        except Exception as e:
            log(f"FAIL switch api: {e}")
            results.append((cid, False)); continue
        p0 = wpos()
        pos_sw = p0[0] if p0 else -1
        time.sleep(WATCH)
        st = api("/api/status")
        moved = (st["active_channel_id"] == cid)
        fr = tv_open_frames()
        drop_caches()
        lp, hp = live_pts(), file_pts_at(0)
        age = (lp - hp) if (lp is not None and hp is not None) else None
        # identity sample: freshest safe region [pos-14MB, pos-8MB],
        # PTS-gated against live (wrap-proof: never anchors at switch point)
        thumb, ident_ok, tries = "SKIP", False, 0
        while not ident_ok and tries < 2:
            p1 = wpos()
            if p1:
                start_mb = (p1[0] - 1593344) / 1048576 - 14.0
                if start_mb < 0.5:
                    start_mb = 0.5
                with open(MOUNT, "rb") as f:
                    f.seek(int(start_mb * 1048576))
                    d = f.read(6 * 1048576)
                sb = os.path.join(TMP, f"rt_{n:02d}_{cid}.bin")
                open(sb, "wb").write(d)
                png = os.path.join(TMP, f"rt_{n:02d}_{cid}.png")
                sh(f"timeout 40 ffmpeg -hide_banner -loglevel error -y -f mpegts -ss 2 "
                   f"-i {sb} -frames:v 1 {png}")
                if os.path.exists(png):
                    thumb = os.path.basename(png)
                    sp = file_pts_at(start_mb)
                    lv = live_pts()
                    if sp and lv and 0 <= (lv - sp) < 120:
                        ident_ok = True
            if not ident_ok:
                tries += 1
                time.sleep(30)
        w = sh("adb shell \"su -c 'tail -n 1 /data/local/tmp/channel_stream.log'\"")
        wlast = w.stdout.strip().splitlines()[-1] if w.stdout.strip() else "N/A"
        age_ok = (age is not None) and -30 <= age < 240
        ok = moved and fr >= 200 and age_ok and ident_ok
        log(f"moved={moved} tvopen={fr}/240 head_age={'%.0f' % age if age is not None and age >= 0 else 'None'}s "
            f"thumb={thumb} ident={ident_ok} w='{wlast[:60]}' => {'PASS' if ok else 'FAIL'}")
        results.append((cid, ok))
    log("LOOP DONE " + " ".join(f"{c}={'P' if o else 'F'}" for c, o in results))
    sys.exit(0 if all(o for _, o in results) else 1)

if __name__ == "__main__":
    main()
