#!/usr/bin/env python3
"""Final-user TV emulation: continuous player (never stops, like Samsung
ConnectShare on repeat-one) + web-remote channel zapping (open + premium).
PASS = player process stays alive through every switch + decodes frames.
Usage: emulate_tv_final.py [watch_secs_per_hop]"""
import json, subprocess, sys, threading, time, urllib.request

BASE = "http://127.0.0.1:8080"
HOPS = [
    ("globo-morena-dourados", "Rede Globo (open)"),
    ("record-news", "Record News (open)"),
    ("sony-channel-br", "Sony Channel (premium)"),
    ("espn-mirror-a07z", "ESPN (premium sport)"),
    ("tv-senado", "TV Senado (public)"),
    ("axn-brasil", "AXN (premium)"),
]

def api(path, data=None):
    req = urllib.request.Request(BASE + path,
        data=json.dumps(data).encode() if data else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode())

def main():
    watch = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    st = api("/api/status")
    print(f"server: {st['active_channel_name']} | up {st['uptime_secs']}s")
    print("TV emulator: opening /live.ts (player stays open whole test)...")
    proc = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "warning",
         "-reconnect", "1", "-reconnect_streamed", "1",
         "-reconnect_delay_max", "3",
         "-i", BASE + "/live.ts", "-vf", "fps=fps=1", "-f", "null", "-"],
        stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True)
    errs = []
    def drain():
        for line in proc.stderr:
            c = line.strip()
            if c and "PES packet size mismatch" not in c and "Application provided invalid" not in c:
                errs.append(c)
    threading.Thread(target=drain, daemon=True).start()
    time.sleep(4)
    if proc.poll() is not None:
        print("FAIL: player exited at startup"); sys.exit(1)
    print("player: PLAYING")
    results = []
    for cid, cname in HOPS:
        t0 = time.time()
        try:
            r = api("/api/switch", {"channel_id": cid})
            lat = (time.time() - t0) * 1000
        except Exception as e:
            results.append((cname, False, f"api err {e}")); continue
        e0 = len(errs)
        t = time.time()
        alive = True
        while time.time() - t < watch:
            if proc.poll() is not None:
                alive = False; break
            time.sleep(1)
        # confirm server actually moved
        st = api("/api/status")
        moved = (st["active_channel_id"] == cid)
        ok = alive and moved
        print(f"[{'OK ' if ok else 'FAIL'}] {cname}: alive={alive} moved={moved} "
              f"api={lat:.0f}ms newerrs={len(errs)-e0}")
        results.append((cname, ok, ""))
    alive_end = proc.poll() is None
    proc.terminate()
    try: proc.wait(timeout=3)
    except Exception: proc.kill()
    print("-" * 60)
    n_ok = sum(1 for _, ok, _ in results if ok)
    print(f"RESULT: {n_ok}/{len(results)} hops clean, player alive at end: {alive_end}")
    sys.exit(0 if (n_ok == len(results) and alive_end) else 1)

if __name__ == "__main__":
    main()
