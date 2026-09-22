#!/usr/bin/env python3
"""
telemetry_agent.py — Phone-side telemetry and remote management agent.
Runs on the rooted Android phone in background.
Periodically collects stream metrics, FUSE read stats, UDC gadget state,
and sends them to the VPS server over HTTPS. Also allows remote execution
of maintenance commands dispatched from the Web Remote.
"""

import os
import sys
import time
import json
import subprocess
import urllib.request
import urllib.error

TELEMETRY_INTERVAL = 2.0  # seconds
SERVER_URL_FILE = "/data/local/tmp/server_url.txt"
PIN_FILE = "/data/local/tmp/pin.txt"
DEFAULT_URL = "https://tv.smre.run.place"

def read_file(path, default=""):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except Exception:
        return default

def get_server_url():
    url = read_file(SERVER_URL_FILE, DEFAULT_URL)
    return url if url else DEFAULT_URL

def get_pin():
    return read_file(PIN_FILE, "1233")

def parse_fuse_log():
    """Extract latest FUSE_READ and ahead-starved info from fuse_direct.log."""
    log_path = "/data/local/tmp/fuse_direct.log"
    if not os.path.exists(log_path):
        return {}
    
    last_read = None
    starved_count = 0
    try:
        with open(log_path, "r", errors="ignore") as f:
            lines = f.readlines()[-30:]
        for line in reversed(lines):
            if "ahead-starved" in line:
                starved_count += 1
            if not last_read and "[FUSE_READ]" in line:
                # e.g.: [FUSE_READ] total=200MB off=215175168 sz=131072 S_write=370MB base=178497540
                parts = line.strip().split()
                data = {}
                for p in parts[1:]:
                    if "=" in p:
                        k, v = p.split("=", 1)
                        data[k] = v
                last_read = data
    except Exception:
        pass
    
    res = {}
    if last_read:
        res["total_read"] = last_read.get("total", "0MB")
        res["off"] = int(last_read.get("off", 0))
        res["sz"] = int(last_read.get("sz", 0))
        res["s_write"] = last_read.get("S_write", "0MB")
        res["base"] = int(last_read.get("base", 0))
    res["starved_events"] = starved_count
    return res

def check_process(name):
    try:
        output = subprocess.check_output(["pgrep", "-f", name], text=True)
        return bool(output.strip())
    except Exception:
        return False

def collect_telemetry():
    channel = read_file("/data/local/tmp/current_channel.txt", "unknown")
    abspos = read_file("/data/local/tmp/stream_abspos.txt", "0")
    
    writer_pos_raw = read_file("/data/local/tmp/writer_pos.txt", "")
    writer_bytes = 0
    if writer_pos_raw:
        parts = writer_pos_raw.split()
        if len(parts) >= 2:
            try:
                writer_bytes = int(parts[1])
            except ValueError:
                pass

    fuse_stats = parse_fuse_log()
    
    gadget_udc = read_file("/sys/kernel/config/usb_gadget/g1/UDC", "")
    if not gadget_udc:
        gadget_udc = read_file("/config/usb_gadget/g1/UDC", "detached")
    
    procs = {
        "fuse_direct": check_process("fuse_direct"),
        "stream_writer": check_process("stream_writer.py"),
        "watchdog": check_process("watch_writer.sh")
    }

    # Calculate lead in seconds and MB if available
    lead_mb = 0.0
    tv_read_off = fuse_stats.get("off", 0)
    fuse_base = fuse_stats.get("base", 0)
    if writer_bytes > 0 and (tv_read_off + fuse_base) > 0:
        lead_bytes = writer_bytes - (tv_read_off + fuse_base)
        lead_mb = round(lead_bytes / (1024 * 1024), 2)

    return {
        "channel": channel,
        "abspos": abspos,
        "writer_bytes": writer_bytes,
        "writer_mb": round(writer_bytes / (1024 * 1024), 2),
        "fuse": fuse_stats,
        "lead_mb": lead_mb,
        "udc": gadget_udc if gadget_udc else "detached",
        "procs": procs,
        "timestamp": time.time()
    }

def run_agent():
    print("[*] Telemetry agent started...")
    while True:
        try:
            server_url = get_server_url()
            pin = get_pin()
            telemetry_data = collect_telemetry()
            
            payload = json.dumps(telemetry_data).encode("utf-8")
            req = urllib.request.Request(
                f"{server_url}/api/telemetry",
                data=payload,
                headers={
                    "Content-Type": "application/json",
                    "X-Auth-PIN": pin
                }
            )
            
            with urllib.request.urlopen(req, timeout=4) as resp:
                if resp.status == 200:
                    resp_data = json.loads(resp.read().decode("utf-8"))
                    cmd = resp_data.get("cmd")
                    if cmd:
                        print(f"[*] Executing remote command: {cmd}")
                        cmd_to_run = cmd if os.geteuid() == 0 else f"su -c {json.dumps(cmd)}"
                        try:
                            out = subprocess.check_output(cmd_to_run, shell=True, stderr=subprocess.STDOUT, timeout=15, text=True)
                        except subprocess.CalledProcessError as cpe:
                            out = f"Error (exit {cpe.returncode}):\n{cpe.output}"
                        except Exception as ex:
                            out = f"Execution error: {ex}"
                        
                        # Post back output
                        res_req = urllib.request.Request(
                            f"{server_url}/api/telemetry_result",
                            data=json.dumps({"cmd": cmd, "output": out}).encode("utf-8"),
                            headers={
                                "Content-Type": "application/json",
                                "X-Auth-PIN": pin
                            }
                        )
                        urllib.request.urlopen(res_req, timeout=4)
        except Exception as e:
            # Silent retry
            pass
        
        time.sleep(TELEMETRY_INTERVAL)

if __name__ == "__main__":
    run_agent()
