# 🤖 USB Stream TV — Agent Execution Guide

> **Purpose**: This document contains EVERYTHING an AI agent needs to understand, deploy, debug, and extend this project. It is the single source of truth, written from hard-won empirical experience with real Samsung hardware.

---

## 📋 Table of Contents

1. [Project Overview](#1-project-overview)
2. [Hardware & Infrastructure](#2-hardware--infrastructure)
3. [Architecture Deep Dive](#3-architecture-deep-dive)
4. [FUSE Direct Engine (fuse_direct.c)](#4-fuse-direct-engine-fuse_directc)
5. [VPS Server (server.py)](#5-vps-server-serverpy)
6. [Phone Scripts](#6-phone-scripts)
7. [Deployment: Full Step-by-Step](#7-deployment-full-step-by-step)
8. [Channel Management](#8-channel-management)
9. [Debugging Playbook](#9-debugging-playbook)
10. [Critical Lessons & Bugs to NEVER Reintroduce](#10-critical-lessons--bugs-to-never-reintroduce)
11. [File Reference](#11-file-reference)
12. [Credentials & Endpoints](#12-credentials--endpoints)

---

## 1. Project Overview

**What it does**: Transforms a rooted Android phone into a virtual USB flash drive that streams live IPTV to a legacy non-smart TV (Samsung Plasma PL51F4000, 2013) via the TV's ConnectShare USB 2.0 port.

**How it works**: A VPS transcodes IPTV HLS streams to MPEG-TS. The phone downloads the stream over Wi-Fi, stores it in a 32 MB RAM ring buffer via FUSE, and presents a virtual FAT32 disk to the TV via USB Mass Storage gadget. The TV sees a file called `TV AO VIVO.ts` and plays it as if it were a regular video file — but the content is infinite live television.

**Data Flow**:
```
IPTV CDN (HLS) → VPS (FFmpeg transcode → MPEG-TS) → Phone (Wi-Fi HTTP)
    → named FIFO → fuse_direct (32MB RAM ring) → USB Mass Storage gadget
    → TV USB ConnectShare → Samsung Media Player → Video Output
```

---

## 2. Hardware & Infrastructure

### TV
| Property | Value |
|----------|-------|
| Model | Samsung Plasma PL51F4000 (2013) |
| USB | ConnectShare USB 2.0, 500mA max |
| Filesystem | FAT32 only (no NTFS, no exFAT) |
| Video Codecs | H.264 Baseline/Main/High up to 1080p 30fps |
| Audio Codecs | AC3 (Dolby Digital), AAC, MP3 |
| Container | MPEG-TS (.ts), AVI, MKV |
| RAM | ~64 MB for media playback buffer |
| ConnectShare behavior | Reads first ~2 MB + offset 8MB for format probe, then sequential reads in 128 KB blocks |

### Phone (USB Dongle)
| Property | Value |
|----------|-------|
| Model | Xiaomi Mi A2 (`jasmine_sprout`) |
| Serial | `4773620` |
| Root | Magisk |
| Kernel | `4.19.288-Scarlet-X-v10.0` |
| Architecture | aarch64 |
| ConfigFS | `/config/usb_gadget/g1` |
| UDC Controller | `a800000.dwc3` |
| Python path | `/data/data/com.termux/files/usr/bin/python3` |
| curl path | `/data/data/com.termux/files/usr/bin/curl` |

> [!CAUTION]
> Standard `ps`, `which` commands don't exist in base Android shell. Use `ps -ef` (Toybox). Always use full paths for Python and curl.

### VPS (Oracle Cloud)
| Property | Value |
|----------|-------|
| SSH alias | `ssh oracle` |
| IP | `129.146.5.64` |
| Architecture | ARM64 (aarch64) |
| Instance | ARM Ampere A1 (PAYG) |
| Docker container | `usb-stream-tv` |
| Container path | `/opt/containers/apps/usb-stream-tv` |
| Public URL | `https://tv.smre.run.place` (Caddy reverse proxy) |
| PIN | `1233` |

### Network
| Device | IP | Notes |
|--------|-----|-------|
| PC (ethernet) | `192.168.1.8` | Development machine |
| Phone (Wi-Fi) | `192.168.1.4` | Different subnet, ping fails |
| VPS | `129.146.5.64` | Oracle Cloud |

> [!WARNING]
> ADB over Wi-Fi does NOT work — `ping 192.168.1.4` from PC returns "Destination Host Unreachable". Use USB ADB cable or VPS `/api/exec` endpoint for remote commands.

### Mark
- **Mark is the person physically at the TV**. He plugs/unplugs the USB cable and reports what the TV screen shows. When the phone is plugged into the TV, ADB is unavailable — use the VPS `/api/exec` endpoint instead.

---

## 3. Architecture Deep Dive

### Data Flow Diagram
```
┌──────────────────┐     HLS/m3u8      ┌──────────────────────────────┐
│  IPTV CDN        │ ──────────────────▶│  VPS Docker Container        │
│  (studut.shop    │                    │  ┌──────────────────────┐    │
│   or cdntvms)    │                    │  │ FFmpeg (-re)         │    │
└──────────────────┘                    │  │ 720p 30fps H.264    │    │
                                        │  │ AC3 48kHz stereo    │    │
                                        │  │ 2200kbps + 192kbps  │    │
                                        │  └────────┬─────────────┘    │
                                        │           │ pipe:1 (MPEG-TS) │
                                        │  ┌────────▼─────────────┐    │
                                        │  │ server.py            │    │
                                        │  │ SeamlessRestamper    │    │
                                        │  │ StreamHub            │    │
                                        │  └────────┬─────────────┘    │
                                        └───────────┼──────────────────┘
                                                    │ HTTP /live.ts
                                        ┌───────────▼──────────────────┐
                                        │  Phone (Xiaomi Mi A2)        │
                                        │  ┌──────────────────────┐    │
                                        │  │ stream_writer.py     │    │
                                        │  │ (HTTP → FIFO)        │    │
                                        │  └────────┬─────────────┘    │
                                        │           │ /data/local/tmp/ │
                                        │           │ live_pipe (FIFO) │
                                        │  ┌────────▼─────────────┐    │
                                        │  │ fuse_direct          │    │
                                        │  │ 32MB RAM ring buffer │    │
                                        │  │ Virtual FAT32 4GB    │    │
                                        │  └────────┬─────────────┘    │
                                        │           │ USB Mass Storage │
                                        │           │ ConfigFS gadget  │
                                        └───────────┼──────────────────┘
                                                    │ USB 2.0 cable
                                        ┌───────────▼──────────────────┐
                                        │  Samsung TV PL51F4000        │
                                        │  ConnectShare USB            │
                                        │  Plays "TV AO VIVO.ts"      │
                                        │  Shows 00:05 / 00:05        │
                                        │  Repetir 1 = infinite loop  │
                                        └──────────────────────────────┘
```

### Three Processes on Phone (all run as root)
1. **`fuse_direct`** — FUSE daemon serving virtual FAT32 disk from RAM
2. **`stream_writer.py`** — Downloads MPEG-TS from VPS, writes to FIFO
3. **`watch_writer.sh`** — Watchdog: restores USB gadget if TV resets it, restarts writer if killed by Android LMK

### Process Boot Order (CRITICAL)
```
1. fuse_direct starts → opens FIFO for read (blocks waiting for writer)
2. stream_writer.py starts → opens FIFO for write (rendezvous with fuse_direct)
3. watch_writer.sh starts → monitors both processes
4. sleep 15 → pre-fill 32 MB ring buffer with ~15s of video
5. echo "$UDC" > "$GADGET/UDC" → activate USB connection to TV
```

> [!CAUTION]
> The FIFO is a rendezvous point. `fuse_direct` MUST start before `stream_writer.py`. If reversed, the writer has no reader and blocks or errors.

---

## 4. FUSE Direct Engine (`fuse_direct.c`)

### Build Command
```bash
aarch64-linux-gnu-gcc -O2 -static fuse_direct.c -o fuse_direct -lpthread
```

### Critical Constants
```c
#define FILE_SIZE   1800000000ULL   // 1.8 GB virtual file (safe for signed 32-bit FAT32)
#define RINGSZ      (32ULL << 20)   // 32 MB RAM ring buffer (~90s at 2.9 Mbps)
#define LEADBACK    (12ULL << 20)   // 12 MB = ~40s behind live (jitter immunity)
#define HDRCACHESZ  (512ULL << 10)  // 512 KB header cache
#define BLOCK_S     10              // 10s max blocking timeout per FUSE_READ
#define DISK_SIZE   ~4.3 GB         // Virtual FAT32 disk size
#define BPS         512             // Bytes per sector
#define SPC         8               // Sectors per cluster (4096 bytes/cluster)
```

### Virtual Disk Layout
```
Sector 0-31:        Reserved (boot sector, FSInfo) — from fat_template.bin
Sector 32-16415:    FAT tables (2 copies × 8192 sectors) — synthesized arithmetically
Sector 16416-16423: Root directory (cluster 2) — from fat_template.bin  
Sector 16424+:      File data (cluster 3+) — served from RAM ring buffer
```

### Key Functions

#### `on_open()` — Called when TV opens the file
- Waits up to 5s for `S_write >= 512 KB` (enough for a decodable start)
- Calls `snap_open_base()` to find optimal start position (PAT + SPS/IDR keyframe)
- Populates `hcache` with first 64 KB from `base` position
- Sets `last_file_read_ms = now_ms()` for fresh playback detection

#### `snap_open_base()` — Finds best video start position
- Scans `[S_write - LEADBACK - 64KB, S_write - LEADBACK]` for a PAT packet (PID 0) followed by an SPS NAL unit (type 7) within 64 KB
- Ensures the TV starts decoding from a clean keyframe, ~40s behind live

#### `serve_disk()` — The heart of the engine
- **Fresh Playback Detection** (the fix that made exit/reopen work):
  When TV reads `foff == 0` after an idle gap >1.5s OR when `base` has fallen out of the ring buffer, immediately re-snap `base` to current live stream with fresh `hcache`. This prevents the 1-2 frame freeze on replay/reconnect.
- **Read pacing**: When TV reads past `S_write` (live frontier), blocks with `wait_step()` up to `deadline_ms`. This creates the "infinite file" illusion — the TV thinks it's buffering a slow USB drive.
- **Stale reads**: When TV re-reads old offsets (behind ring buffer), serves valid cyclic TS data from the ring rather than null/corrupt data.
- **Lazy rebase**: Tracks sequential reads and rebases when data becomes stale (3-clause rule: consecutive stale reads < 2MB trigger rebase after 3 sequential).

> [!IMPORTANT]
> **NEVER remove the blocking waits in serve_disk for sequential reads near the frontier.** A previous attempt to make all reads non-blocking caused the TV to read the entire 1.8 GB file in seconds and freeze. The blocking is what creates the "infinite playback at 1x speed" behavior.

---

## 5. VPS Server (`server.py`)

### FFmpeg Transcoding Parameters
```
Video:  libx264, 1280×720 30fps, ultrafast, zerolatency
        bitrate=2200k, maxrate=2600k, bufsize=1300k
        GOP=30 (1 keyframe/second), profile=main, level=4.1
        repeat-headers=1 (SPS/PPS in every IDR)
Audio:  AC3 (Dolby Digital), 48000 Hz, stereo, 192 kbps
PIDs:   Video=256 (0x100), Audio=257 (0x101), PMT=4096 (0x1000)
Muxing: mpegts, muxdelay=0, muxpreload=0, genpts, discardcorrupt
Input:  -re (real-time), reconnect, allowed_segment_extensions=ALL
```

### SeamlessRestamper
Ensures timestamp continuity across channel switches:
- **Continuity Counter (CC)**: Sequential 0-15 per PID
- **PTS/DTS**: Anchored to `target_base_pts = 270000` (3s margin) on first video PES; `start_new_channel()` sets target to `max_pts_seen + 3000`
- **PCR**: 27 MHz clock, offset-adjusted
- **Jump smoothing**: Re-bases on jumps < -1s or > +5s

### StreamHub
- Single FFmpeg subprocess, broadcasts to subscriber queues
- `_reader_loop()`: reads 65,424-byte chunks (348 TS packets), processes through restamper
- Reconnects automatically if FFmpeg dies (zombie detection fixed)
- **Keepalive**: Every 0.2s, if no real data for >0.6s, sends 32 KB null TS packets (PID 0x1FFF) to prevent TV USB buffer starvation
- **Standby**: After 90s with no listeners, kills FFmpeg to save IPTV bandwidth; wakes on new subscriber

### Channel Switching (Make-Before-Break)
1. New FFmpeg starts for target channel
2. Old channel **continues streaming** during connection (up to 8s timeout)
3. On first successful read from new channel: atomic swap of `self.proc`
4. Restamper starts new epoch seamlessly
5. Old FFmpeg killed

### HTTP Endpoints
| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/live.ts` | GET | No | Live MPEG-TS stream |
| `/api/status` | GET | No | JSON status (channel, listeners, telemetry) |
| `/api/switch` | POST | PIN | Switch channel: `{"channel_id": "..."}` |
| `/api/channels` | GET | No | List all channels |
| `/api/exec` | POST | PIN | Execute command on phone: `{"cmd": "..."}` |
| `/api/telemetry` | POST | PIN | Receive phone telemetry |
| `/fuse_direct_arm_verified` | GET | No | Download verified FUSE binary |
| `/start_clean.sh` | GET | No | Download start script |
| `/*.sh` | GET | No | Download any .sh file |

---

## 6. Phone Scripts

### `start_clean.sh` — Clean Restart
1. Kills all processes (`fuse_direct`, `stream_writer.py`, `watch_writer.sh`)
2. Renames `*.new` files (update mechanism: download new binary as `.new`, restart picks it up)
3. Unmounts FUSE, recreates FIFO
4. Starts `fuse_direct` → waits 1s → configures USB gadget
5. Starts `stream_writer.py` and `watch_writer.sh`
6. **Sleeps 15s** for buffer pre-fill
7. Activates UDC (connects USB to TV)

### `watch_writer.sh` — Watchdog
- Runs every 5s with `oom_score_adj=-1000` (immune to Android Low Memory Killer)
- **USB Gadget Recovery**: If Android resets USB to MTP/charging (TV power cycle), restores Mass Storage gadget with `LIVETV` inquiry string
- **Writer Recovery**: Every 15s, checks if `stream_writer.py` is alive. If dead, queries `/api/status` for active channel and restarts via `on_channel_switch.sh`

### `stream_writer.py` — Stream Downloader
- Connects to `https://tv.smre.run.place/live.ts`
- Writes to named FIFO `/data/local/tmp/live_pipe` in 65,424-byte chunks
- Natural backpressure: FIFO write blocks when ring is full
- Auto-reconnects on HTTP disconnect or broken pipe
- Writes heartbeat to `writer_pos.txt` every 15s

### `telemetry_agent.py` — Remote Monitoring
- Reports phone state to VPS every 2s
- Executes remote commands received via `/api/telemetry` response
- Parses `fuse_direct.log` for live stats

---

## 7. Deployment: Full Step-by-Step

### Prerequisites
- [ ] Phone rooted with Magisk, Termux installed with Python 3
- [ ] `aarch64-linux-gnu-gcc` cross-compiler on dev machine
- [ ] VPS with Docker and Caddy reverse proxy
- [ ] GitHub repo: `Tatarotus/usb-stream-tv`

### Step 1: Build the FUSE Binary
```bash
cd /home/sam/Code/usb-stream-tv
aarch64-linux-gnu-gcc -O2 -static fuse_direct.c -o fuse_direct -lpthread
sha256sum fuse_direct  # Save this hash!
cp fuse_direct fuse_direct_arm_verified
```

### Step 2: Generate FAT32 Template
```bash
python3 gen_template.py
# Creates fat_template.bin (5,676 bytes)
```

### Step 3: Deploy VPS
```bash
scp server.py channels.json channels_deploy.json oracle:/opt/containers/apps/usb-stream-tv/
scp fuse_direct fuse_direct_arm_verified fat_template.bin start_clean.sh oracle:/opt/containers/apps/usb-stream-tv/
ssh oracle "cd /opt/containers/apps/usb-stream-tv && sudo docker compose up -d --build"
```

Verify:
```bash
curl -s https://tv.smre.run.place/api/status | python3 -m json.tool
curl -s -m 3 https://tv.smre.run.place/live.ts | ffprobe -i pipe:0
# Should show: 1280x720 H.264 + AC3 48kHz stereo
```

### Step 4: Deploy to Phone (via ADB)
```bash
adb push fuse_direct /data/local/tmp/fuse_direct
adb push fat_template.bin /data/local/tmp/fat_template.bin
adb push stream_writer.py /data/local/tmp/stream_writer.py
adb push start_clean.sh /data/local/tmp/start_clean.sh
adb push watch_writer.sh /data/local/tmp/watch_writer.sh
adb push on_channel_switch.sh /data/local/tmp/on_channel_switch.sh
adb push telemetry_agent.py /data/local/tmp/telemetry_agent.py
adb shell "su -c 'chmod 755 /data/local/tmp/fuse_direct /data/local/tmp/start_clean.sh /data/local/tmp/watch_writer.sh /data/local/tmp/on_channel_switch.sh'"
echo "1233" | adb shell "su -c 'cat > /data/local/tmp/pin.txt'"
echo "https://tv.smre.run.place" | adb shell "su -c 'cat > /data/local/tmp/server_url.txt'"
```

### Step 5: Deploy to Phone (via VPS API, no ADB needed)
```bash
# Download binary from VPS to phone
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "/data/data/com.termux/files/usr/bin/curl -k -s https://tv.smre.run.place/fuse_direct_arm_verified -o /data/local/tmp/fuse_direct.new && sha256sum /data/local/tmp/fuse_direct.new"}' \
  https://tv.smre.run.place/api/exec
```

### Step 6: Start Streaming
```bash
# Via ADB:
adb shell "su -c 'nohup sh /data/local/tmp/start_clean.sh > /data/local/tmp/start_clean.log 2>&1 &'"

# Via VPS API:
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "nohup sh /data/local/tmp/start_clean.sh > /data/local/tmp/start_clean.log 2>&1 & echo LAUNCHED"}' \
  https://tv.smre.run.place/api/exec
```

Wait 18 seconds, then verify:
```bash
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "cat /data/local/tmp/start_clean.log; echo ---; sha256sum /data/local/tmp/fuse_direct; echo ---; cat /data/local/tmp/writer_pos.txt; echo ---; cat /config/usb_gadget/g1/UDC"}' \
  https://tv.smre.run.place/api/exec
```

Expected output:
```
STARTED_OK UDC=a800000.dwc3
---
<hash>  /data/local/tmp/fuse_direct
---
<bytes> <bytes> globo-morena-dourados <time>
---
a800000.dwc3
```

### Step 7: TV Setup
1. Plug USB cable from phone into TV USB port
2. TV shows "Reproduza vídeos do dispositivo USB" → press Enter
3. Navigate: **LIVETV → Vídeos → TV AO VIVO.ts → PLAY**
4. Press **Tools** on remote → **Modo de Repetição** → **Repetir 1**
5. Live TV should appear within 3-5 seconds (waiting for keyframe)

---

## 8. Channel Management

### Switch Channel via Web
```bash
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"channel_id": "globo-morena-dourados"}' \
  https://tv.smre.run.place/api/switch
```

### List Channels
```bash
curl -s https://tv.smre.run.place/api/channels | python3 -m json.tool
```

### Working CDN Channels (no IPTV account needed)
| ID | Name | URL |
|----|------|-----|
| `globo-morena-dourados` | Rede Globo (TV Morena) | `https://media2.cdntvms.com.br/tv_morena_dorados/index.m3u8` |
| `tv-gazeta-sp` | TV Gazeta SP | `http://45.162.64.114/GAZETA/index.m3u8` |
| `rede-tv-nacional` | RedeTV! Nacional | `http://45.162.64.114/REDE_TV/index.m3u8` |

### IPTV Account (studut.shop)
- Server: `studut.shop:80`
- User: `0939303360` / Pass: `3811610453`
- Max 1 connection (DO NOT open multiple FFmpeg instances)
- User-Agent must be `Mozilla/5.0` for studut.shop URLs
- URL format: `http://studut.shop:80/live/0939303360/3811610453/{STREAM_ID}.m3u8`

> [!WARNING]
> The studut.shop channels use non-standard HLS segment extensions. FFmpeg requires `-allowed_segment_extensions ALL -extension_picky 0` or it fails with `URL is not in allowed_segment_extensions`.

---

## 9. Debugging Playbook

### Check Full System Status
```bash
curl -s -H "X-Auth-PIN: 1233" https://tv.smre.run.place/api/status | python3 -m json.tool
```

### Check FUSE Log (via VPS API)
```bash
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "tail -n 30 /data/local/tmp/fuse_direct.log"}' \
  https://tv.smre.run.place/api/exec
```

### Check Stream Log
```bash
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "tail -n 20 /data/local/tmp/channel_stream.log"}' \
  https://tv.smre.run.place/api/exec
```

### Check VPS Docker Logs
```bash
ssh oracle "sudo docker logs --tail 30 usb-stream-tv"
```

### Symptom → Diagnosis Table

| Symptom | Cause | Fix |
|---------|-------|-----|
| **TV shows "Nenhum arquivo de vídeo"** | FUSE not running, or gadget not configured | Run `start_clean.sh` |
| **TV shows "Carregando..." forever (black screen)** | FFmpeg died (zombie), only null packets flowing | Check `ssh oracle "pgrep -a ffmpeg"`. If `<defunct>`, restart container: `ssh oracle "cd /opt/containers/apps/usb-stream-tv && sudo docker compose restart"` |
| **Plays 1-2 frames then freezes on reopen** | Stale hcache/base not refreshed | Ensure `fuse_direct` has the fresh-playback-detection code at `foff == 0` (commit `652923b`) |
| **Stuttering: plays 30s, freezes 3min, repeat** | Blocking waits too aggressive, or 1080p bitrate too high | Ensure 720p 2200k (not 1080p 3500k). Ensure blocking code is present (not removed). |
| **Sound but no picture** | AC3 audio works but video PID/codec mismatch | Check `ffprobe` output: must be `H.264 Constrained Baseline` with PID 256 |
| **"Repetir 1" not available** | File hasn't started playing yet | Play the file first, THEN set Repetir 1 |
| **Gadget keeps resetting to MTP** | Android USB manager fights ConfigFS | `watch_writer.sh` handles this automatically every 5s |
| **Writer dies randomly** | Android Low Memory Killer | `watch_writer.sh` restarts it; `oom_score_adj=-1000` protects watchdog |
| **PTS_JUMP warnings in server_events.log** | Source stream has discontinuities | Normal for HLS sources; SeamlessRestamper handles these |
| **Channel switch timeout** | Source URL dead or rate-limited | Check `curl -I <url>` from VPS. Try different channel. |
| **`S_write=0MB` in fuse log** | Writer hasn't connected to FIFO yet | Wait for `[*] fifo reader connected` message |

### Verify Stream is Valid
```bash
# From dev machine:
curl -s -m 5 https://tv.smre.run.place/live.ts | python3 -c '
import sys
data = sys.stdin.buffer.read()
pids = {}
for i in range(0, len(data) - 188, 188):
    if data[i] != 0x47: continue
    pid = ((data[i+1] & 0x1f) << 8) | data[i+2]
    pids[pid] = pids.get(pid, 0) + 1
print("Bytes:", len(data), "PIDs:", pids)
'
```

**Expected PIDs**: `{0: PAT, 256: Video, 257: Audio, 4096: PMT, 17: SDT}`
**Bad sign**: Only `{8191: ...}` = all null packets, FFmpeg is dead!

---

## 10. Critical Lessons & Bugs to NEVER Reintroduce

> [!CAUTION]
> These lessons were learned through hours of debugging on real hardware. Violating ANY of these will break the system.

### 1. NEVER remove blocking waits from `serve_disk()` for sequential reads
The blocking at the live frontier (lines with `while (start >= S_write && now_ms() < deadline_ms)`) is what creates the "infinite file at 1x speed" illusion. Removing it causes the TV to read the entire 1.8 GB in seconds, hit EOF, and freeze.

### 2. NEVER increase resolution above 720p
The TV's hardware decoder handles 720p at 2.2 Mbps perfectly. At 1080p 3.5 Mbps, the USB 2.0 bandwidth + TV RAM cache cannot keep up, causing stuttering.

### 3. NEVER use FILE_SIZE > 1,800,000,000
Values above ~2.1 billion trigger signed 32-bit integer overflow in the TV's FAT32 implementation. `0xFFFFFFFF` (4.29 GB) causes `DIR_FileSize = -24,576 bytes` and the TV shows "unsupported file".

### 4. NEVER start stream_writer.py BEFORE fuse_direct
The FIFO is a rendezvous. fuse_direct opens for read, stream_writer opens for write. Reversed order = deadlock or error.

### 5. NEVER remove hcache refresh from on_open() and foff==0 detection
Without refreshing hcache on every file reopen, the TV gets stale headers from hours ago at offset 0, followed by a massive timestamp jump — causing the 1-2 frame freeze on replay.

### 6. NEVER use AAC audio for this TV model
Samsung PL51F4000 handles AC3 (Dolby Digital) natively. AAC sometimes causes audio desync or "unsupported format". Always use `-c:a ac3 -b:a 192k -ar 48000 -ac 2`.

### 7. NEVER have multiple FFmpeg instances hitting the same IPTV source
studut.shop allows max 1 connection. Multiple instances trigger Cloudflare 429/ban.

### 8. ALWAYS wait 15 seconds after starting processes before activating UDC
The TV reads the first sectors immediately on USB connection. If the ring buffer is empty (S_write=0), the TV sees null data and shows "Nenhum arquivo de vídeo". The 15s pre-fill ensures ~4 MB of valid video is ready.

### 9. ALWAYS use `-allowed_segment_extensions ALL` for HLS sources
Modern IPTV providers (studut.shop) use non-standard segment extensions that FFmpeg 7.x rejects by default.

### 10. The `fuse_direct` binary must be statically linked
Android doesn't have standard Linux shared libraries. `-static` is mandatory.

---

## 11. File Reference

### Core Files (deployed to phone)
| File | Purpose |
|------|---------|
| `fuse_direct` | ARM64 static binary — FUSE virtual disk engine |
| `fat_template.bin` | Static FAT32 metadata (boot, FSInfo, root dir) |
| `stream_writer.py` | Downloads MPEG-TS from VPS, writes to FIFO |
| `start_clean.sh` | Clean restart script |
| `watch_writer.sh` | Watchdog for USB gadget and writer process |
| `on_channel_switch.sh` | Channel switching on phone |
| `telemetry_agent.py` | Remote monitoring and command execution |

### Core Files (deployed to VPS)
| File | Purpose |
|------|---------|
| `server.py` | HTTP server, FFmpeg management, restamper |
| `channels.json` | CDN channel definitions (42 channels) |
| `channels_deploy.json` | IPTV provider channel definitions (36 channels) |
| `Dockerfile` | Docker image definition |
| `compose.yaml` | Docker Compose service definition |

### Source Files (dev machine only)
| File | Purpose |
|------|---------|
| `fuse_direct.c` | FUSE engine source code |
| `gen_template.py` | FAT32 template generator |
| `fuse_direct_SPEC.md` | FUSE specification document |

### Documentation
| File | Purpose |
|------|---------|
| `README.md` | User-facing project overview |
| `SYSTEM_HANDOVER_AND_ARCHITECTURE.md` | Architecture and hard-won lessons |
| `AGENT_GUIDE.md` | **This file** — complete agent execution guide |

---

## 12. Credentials & Endpoints

### VPS Access
```bash
ssh oracle                          # SSH into VPS
sudo docker exec -it usb-stream-tv bash  # Enter container
```

### API Endpoints
```bash
# Status
curl -s https://tv.smre.run.place/api/status

# Switch channel
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"channel_id": "globo-morena-dourados"}' https://tv.smre.run.place/api/switch

# Execute command on phone
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "uptime"}' https://tv.smre.run.place/api/exec

# Download live stream
curl -s https://tv.smre.run.place/live.ts | mpv -
```

### GitHub
```
Repository: Tatarotus/usb-stream-tv
Branch: main
Latest commit: 652923b
```

### IPTV Account
```
Provider: studut.shop:80
User: 0939303360
Password: 3811610453
Max connections: 1
```

### TV Remote Control PIN
```
PIN: 1233
```

---

> **Last updated**: 2026-09-23 by Antigravity agent, after empirically verifying smooth playback including exit/reopen, cable disconnect/reconnect, and channel switching on Samsung PL51F4000.
