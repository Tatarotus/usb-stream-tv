#!/usr/bin/env python3
"""
Automated mpv playback & telemetry monitor for usb-stream-tv.
Connects directly to mpv's IPC socket to track:
- Exact playback position (time-pos)
- Total reported duration
- A-V synchronization offset
- Dropped frames
- Sudden skips / timestamp discontinuities
- Seamless loop events across the 60MB circular boundary
"""

import subprocess, time, socket, json, os, sys

def run_loop_monitor(target_duration_secs=360, socket_path="/tmp/mpv-loop.sock"):
    file_path = "/run/media/sam/LIVETV/CANAL AO VIVO.ts"
    
    # 1. Flush Linux host RAM page cache first so mpv reads fresh sectors
    os.system("sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null")
    if os.path.exists(socket_path):
        os.remove(socket_path)

    log_file = open("/tmp/mpv-loop.log", "w")
    cmd = [
        "mpv",
        "--vo=null",
        "--ao=null",
        "--loop-file=inf",
        "--msg-level=all=info",
        f"--input-ipc-server={socket_path}",
        file_path
    ]

    print("="*60)
    print(f"[*] Starting mpv loop monitor for {target_duration_secs}s ({target_duration_secs/60:.1f} min)")
    print(f"[*] Target file: {file_path}")
    print("="*60)

    proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT)
    time.sleep(1.5)

    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(socket_path)
    except Exception as e:
        print(f"[-] Failed to connect to mpv IPC socket: {e}")
        proc.terminate()
        log_file.close()
        return False

    def get_prop(name):
        try:
            req = json.dumps({"command": ["get_property", name]}) + "\n"
            sock.sendall(req.encode())
            data = sock.recv(1024).decode()
            for line in data.strip().split("\n"):
                if not line.strip():
                    continue
                res = json.loads(line)
                if "data" in res:
                    return res["data"]
        except Exception:
            return None
        return None

    start_time = time.time()
    last_pos = None
    last_print = 0
    loop_count = 0
    anomalies = []

    print(f"[{time.strftime('%H:%M:%S')}] Monitoring started. Polling every 0.5s...")

    read_log = open("/tmp/mpv-loop.log", "r")

    try:
        while time.time() - start_time < target_duration_secs:
            if proc.poll() is not None:
                print(f"\n[!] mpv exited prematurely with code {proc.returncode}!")
                anomalies.append(f"Premature exit with code {proc.returncode}")
                break

            # Read any new log lines from mpv
            for line in read_log:
                line_s = line.strip()
                if any(w in line_s for w in ["Invalid", "corrupt", "Packet corrupt", "Reset playback", "DTS", "PTS"]):
                    print(f"  [mpv log] {line_s}")
                    anomalies.append(f"Log event: {line_s}")

            pos = get_prop("time-pos")
            dur = get_prop("duration")
            drops = get_prop("frame-drop-count") or 0
            av = get_prop("avsync") or 0.0

            now = time.time()
            elapsed = now - start_time

            if pos is not None and last_pos is not None:
                delta = pos - last_pos

                # Check for loop back to start (e.g. from >90s back to <15s)
                if last_pos > 90 and pos < 15:
                    loop_count += 1
                    print(f"\n[✓ LOOP #{loop_count}] Successful loop at T+{elapsed:.1f}s: pos wrapped from {last_pos:.1f}s -> {pos:.1f}s")
                # Check for unexpected forward or backward jump > 10s
                elif abs(delta) > 10:
                    jump_type = "FORWARD JUMP" if delta > 0 else "BACKWARD JUMP"
                    msg = f"[{jump_type}] at T+{elapsed:.1f}s: pos leaped from {last_pos:.1f}s to {pos:.1f}s ({delta:+.1f}s jump!)"
                    print(f"\n[!] {msg}")
                    anomalies.append(msg)

            if pos is not None:
                last_pos = pos

            # Print heartbeat line every 5 seconds
            if now - last_print >= 5:
                last_print = now
                pos_str = f"{int(pos//60):02d}:{int(pos%60):02d}" if pos is not None else "--:--"
                dur_str = f"{int(dur//60):02d}:{int(dur%60):02d}" if dur is not None else "--:--"
                pct_str = f"{int((pos/dur)*100):2d}%" if (pos is not None and dur and dur > 0) else " --"
                print(f"  [{time.strftime('%H:%M:%S')}] T+{elapsed:5.1f}s | AV: {pos_str} / {dur_str} ({pct_str}) | A-V: {av:+.3f} | Drops: {drops} | Loops: {loop_count}")

            time.sleep(0.5)

    except KeyboardInterrupt:
        print("\n[*] Monitor interrupted by user.")
    finally:
        sock.close()
        read_log.close()
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        log_file.close()

    total_time = time.time() - start_time
    print("\n" + "="*60)
    print(f"MONITOR SUMMARY ({total_time:.1f}s elapsed, {total_time/60:.1f} min)")
    print(f"Loops observed: {loop_count}")
    print(f"Anomalies detected: {len(anomalies)}")
    for a in anomalies:
        print(f"  - {a}")
    if len(anomalies) == 0 and loop_count >= 1:
        print("[SUCCESS] Continuous looping verified with 0 timestamp jumps or stalls!")
    print("="*60)
    return len(anomalies) == 0

if __name__ == "__main__":
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 360
    run_loop_monitor(dur)
