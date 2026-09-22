#!/usr/bin/env python3
import subprocess, time, sys

def run_test(duration_secs=300):
    cmd = [
        "mpv",
        "--vo=null",
        "--ao=null",
        "--loop-file=inf",
        "--term-status-msg=STATUS_METRIC: pos=${time-pos} dur=${duration} drops=${frame-drop-count} avsync=${avsync}",
        "/run/media/sam/LIVETV/CANAL AO VIVO.ts"
    ]
    
    print(f"[*] Starting automated mpv watcher for {duration_secs}s...")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    start = time.time()
    last_pos = None
    loop_count = 0
    anomalies = []
    
    try:
        while time.time() - start < duration_secs:
            line = p.stdout.readline()
            if not line:
                if p.poll() is not None:
                    print(f"[!] mpv process exited unexpectedly with code {p.returncode}")
                    break
                continue
            line = line.strip()
            
            if "STATUS_METRIC:" in line:
                # Extract metrics
                parts = line.split("STATUS_METRIC:")[1].strip().split()
                metrics = dict(item.split("=") for item in parts if "=" in item)
                try:
                    pos = float(metrics.get("pos", 0))
                    dur = float(metrics.get("dur", 0))
                    drops = int(metrics.get("drops", 0))
                    
                    if last_pos is not None:
                        delta = pos - last_pos
                        # Check for loop back to start
                        if last_pos > 120 and pos < 15:
                            loop_count += 1
                            elapsed = time.time() - start
                            print(f"[✓ LOOP #{loop_count}] Loop occurred cleanly at T+{elapsed:.1f}s (pos went from {last_pos:.1f}s -> {pos:.1f}s)")
                        # Check for forward jump > 10s without normal time progression
                        elif delta > 15:
                            elapsed = time.time() - start
                            anomaly = f"FORWARD JUMP: at T+{elapsed:.1f}s, pos jumped from {last_pos:.1f}s to {pos:.1f}s (dur={dur:.1f}s)"
                            print(f"[!] {anomaly}")
                            anomalies.append(anomaly)
                    last_pos = pos
                except ValueError:
                    pass
            elif any(err in line for err in ["Invalid audio PTS", "Packet corrupt", "DTS", "Reset playback", "error"]):
                elapsed = time.time() - start
                print(f"[T+{elapsed:6.1f}s EVENT] {line}")
                anomalies.append(f"T+{elapsed:.1f}s: {line}")
                
    except KeyboardInterrupt:
        print("\n[*] Stopped by user.")
    finally:
        p.terminate()
        try:
            p.wait(timeout=2)
        except subprocess.TimeoutExpired:
            p.kill()
            
    elapsed = time.time() - start
    print("\n" + "="*50)
    print(f"Test finished after {elapsed:.1f}s ({elapsed/60:.1f} min)")
    print(f"Total loops observed: {loop_count}")
    print(f"Total anomalies logged: {len(anomalies)}")
    print("="*50)

if __name__ == "__main__":
    dur = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    run_test(dur)
