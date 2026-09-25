#!/usr/bin/env python3
"""
Continuous Stream Reliability Tester (3-Hour Automated Verification)
Monitors playback of https://tv.smre.run.place/live.ts via mpv IPC.
Logs milestones, server health, frame drops, and catches any EOF/crash.
"""

import sys
import os
import time
import json
import socket
import argparse
import subprocess
import urllib.request
from datetime import datetime, timezone

IPC_SOCK = "/tmp/mpv_stream_test.sock"

def query_mpv_property(sock, prop_name):
    cmd = json.dumps({"command": ["get_property", prop_name]}).encode("utf-8") + b"\n"
    try:
        sock.sendall(cmd)
        buf = b""
        while not buf.endswith(b"\n"):
            part = sock.recv(1024)
            if not part:
                break
            buf += part
        if not buf:
            return None
        res = json.loads(buf.decode("utf-8", errors="replace").strip())
        if res.get("error") == "success":
            return res.get("data")
    except Exception:
        pass
    return None

def fetch_api_status(api_url="https://tv.smre.run.place/api/status"):
    try:
        req = urllib.request.Request(api_url, headers={"User-Agent": "StreamTester/1.0"})
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return {"error": str(e)}

def format_hms(seconds):
    h = int(seconds // 3600)
    m = int((seconds % 3600) // 60)
    s = int(seconds % 60)
    return f"{h:02d}:{m:02d}:{s:02d}"

def main():
    parser = argparse.ArgumentParser(description="3-Hour Stream Reliability Tester")
    parser.add_argument("--duration", type=int, default=10800, help="Target duration in seconds (default: 10800 = 3h)")
    parser.add_argument("--url", default="https://tv.smre.run.place/live.ts", help="Stream URL")
    parser.add_argument("--status-file", default="test_stream_status.json", help="Status JSON output file")
    parser.add_argument("--log-file", default="test_stream_3h.log", help="Human-readable milestone log")
    parser.add_argument("--mpv-log", default="test_mpv.log", help="mpv raw log file")
    args = parser.parse_args()

    target_sec = args.duration
    stream_url = args.url

    if os.path.exists(IPC_SOCK):
        try:
            os.unlink(IPC_SOCK)
        except OSError:
            pass

    log_fp = open(args.log_file, "a", buffering=1, encoding="utf-8")
    start_iso = datetime.now(timezone.utc).isoformat()
    start_ts = time.time()

    def log(msg):
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{now_str}] {msg}"
        print(line, flush=True)
        log_fp.write(line + "\n")

    log("=" * 70)
    log(f"Iniciando teste contínuo de {format_hms(target_sec)} ({target_sec}s)")
    log(f"Stream URL: {stream_url}")
    log(f"IPC Socket: {IPC_SOCK} | MPV Log: {args.mpv_log}")
    log("=" * 70)

    # Launch mpv process
    mpv_cmd = [
        "mpv",
        "--vo=null",
        "--ao=null",
        f"--input-ipc-server={IPC_SOCK}",
        f"--log-file={args.mpv_log}",
        "--msg-level=all=warn,cplayer=info",
        stream_url
    ]

    proc = subprocess.Popen(mpv_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)

    # Connect to IPC
    ipc_sock = None
    for attempt in range(10):
        if os.path.exists(IPC_SOCK):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect(IPC_SOCK)
                ipc_sock = s
                break
            except Exception:
                time.sleep(0.5)
        else:
            time.sleep(0.5)

    if not ipc_sock:
        log("[ERRO] Falha ao conectar ao IPC do MPV!")
        proc.terminate()
        sys.exit(1)

    log("[✓] MPV iniciado e conectado ao IPC com sucesso.")

    last_milestone_log = 0.0
    last_api_fetch = 0.0
    server_status = {}
    last_playback_time = 0.0
    dropped_frames = 0
    cache_dur = 0.0

    try:
        while True:
            now = time.time()
            elapsed = now - start_ts

            # Check if mpv exited unexpectedly
            rc = proc.poll()
            if rc is not None:
                log(f"[FALHA CRÍTICA] MPV encerrou inesperadamente com código rc={rc} após {format_hms(elapsed)} ({elapsed:.1f}s)!")
                
                # Read last lines of mpv log
                last_mpv_lines = []
                if os.path.exists(args.mpv_log):
                    try:
                        with open(args.mpv_log, "r", encoding="utf-8", errors="replace") as f:
                            lines = f.readlines()
                            last_mpv_lines = [l.strip() for l in lines[-30:]]
                    except Exception:
                        pass

                failure_data = {
                    "status": "FAILED_EOF",
                    "exit_code": rc,
                    "elapsed_secs": round(elapsed, 1),
                    "target_duration_secs": target_sec,
                    "last_playback_time": last_playback_time,
                    "last_mpv_lines": last_mpv_lines,
                    "end_time": datetime.now(timezone.utc).isoformat()
                }
                with open(args.status_file, "w", encoding="utf-8") as f:
                    json.dump(failure_data, f, indent=2)

                log("Últimas linhas do log do MPV:")
                for line in last_mpv_lines[-10:]:
                    log(f"  > {line}")

                sys.exit(1)

            # Query mpv metrics
            pb_time = query_mpv_property(ipc_sock, "playback-time")
            if pb_time is not None:
                last_playback_time = pb_time

            drops = query_mpv_property(ipc_sock, "frame-drop-count")
            if drops is not None:
                dropped_frames = drops

            cache = query_mpv_property(ipc_sock, "demuxer-cache-duration")
            if cache is not None:
                cache_dur = cache

            # Fetch API status periodically
            if now - last_api_fetch >= 15.0:
                server_status = fetch_api_status()
                last_api_fetch = now

            pct = min(100.0, (elapsed / target_sec) * 100.0)

            # Update JSON status file
            status_data = {
                "status": "RUNNING",
                "start_time": start_iso,
                "last_update": datetime.now(timezone.utc).isoformat(),
                "target_duration_secs": target_sec,
                "elapsed_secs": round(elapsed, 1),
                "elapsed_hms": format_hms(elapsed),
                "remaining_secs": round(max(0, target_sec - elapsed), 1),
                "progress_percent": round(pct, 2),
                "mpv_playback_time": round(last_playback_time, 1),
                "dropped_frames": dropped_frames,
                "demuxer_cache_secs": round(cache_dur, 2),
                "server_health": server_status.get("stream_health", "unknown"),
                "server_bitrate_kbps": server_status.get("stream_bitrate_kbps", 0),
                "server_slate_mode": server_status.get("slate_mode", False),
                "server_listeners": server_status.get("listeners", 0),
                "active_channel": server_status.get("active_channel_name", "unknown")
            }

            temp_status = args.status_file + ".tmp"
            with open(temp_status, "w", encoding="utf-8") as f:
                json.dump(status_data, f, indent=2)
            os.replace(temp_status, args.status_file)

            # Milestone log every 60 seconds
            if now - last_milestone_log >= 60.0:
                health = server_status.get("stream_health", "ok")
                kbps = server_status.get("stream_bitrate_kbps", 0)
                ch = server_status.get("active_channel_name", "Live")
                log(f"[PROGRESSO {pct:.1f}%] {format_hms(elapsed)}/{format_hms(target_sec)} | "
                    f"MPV: {last_playback_time:.1f}s | Drops: {dropped_frames} | Cache: {cache_dur:.1f}s | "
                    f"VPS: {health} ({kbps:.0f} kbps) ch='{ch}'")
                last_milestone_log = now

            # Check for completion
            if elapsed >= target_sec:
                log("=" * 70)
                log(f"[✓ SUCESSO ABSOLUTO] Teste de {format_hms(target_sec)} concluído sem interrupções nem EOF!")
                log(f"Total executado: {elapsed:.1f}s | MPV Time: {last_playback_time:.1f}s | Drops: {dropped_frames}")
                log("=" * 70)

                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()

                status_data["status"] = "COMPLETED_SUCCESS"
                status_data["completed_at"] = datetime.now(timezone.utc).isoformat()
                with open(args.status_file, "w", encoding="utf-8") as f:
                    json.dump(status_data, f, indent=2)

                sys.exit(0)

            time.sleep(2.0)

    except KeyboardInterrupt:
        log("[!] Teste interrompido pelo usuário via KeyboardInterrupt.")
        proc.terminate()
        sys.exit(130)
    finally:
        if ipc_sock:
            try:
                ipc_sock.close()
            except Exception:
                pass
        if proc.poll() is None:
            proc.terminate()
        log_fp.close()

if __name__ == "__main__":
    main()
