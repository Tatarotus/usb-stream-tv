# USB Stream TV — Complete System Memory & Architecture v2

> **For the next agent/session**: Read this document AND the skill file at `/home/sam/.agents/skills/usb-stream-tv/SKILL.md` before doing anything.

---

## 1. What This Project Does

Streams live IPTV channels and movies to a **Samsung Plasma TV (PL51F4000)** that has NO network, NO smart apps — only a USB port (ConnectShare). A rooted Android device plugged into the TV's USB port emulates a FAT32 flash drive containing `.ts` video files. When the user selects a file on the TV, the system detects which channel was chosen and streams live data into that file's disk sectors in real-time.

---

## 2. The Fundamental Problem With the Current Architecture

### Current Data Flow (v1 — BROKEN)
```
Server (FFmpeg) ─HTTP─→ curl (on phone) ─pipe─→ dd (writes to tv_stream.img sectors)
                                                        ↑ RACE CONDITION ↑
TV ─USB SCSI READ─→ f_mass_storage ─→ FUSE sensor ─→ pread(tv_stream.img) ─→ TV
```

**Why it lags**: The TV reads from the disk image via USB at ~480 Mbps. The `curl|dd` pipeline writes to the same disk image at ~3-5 Mbps (internet speed). The TV's read pointer outruns the write pointer within milliseconds. The TV reads stale/empty data → corruption, freezing, "arquivo não suportado".

Every "fix" attempted was a band-aid on this fundamental race:
- Pre-injecting headers → delays the problem by ~2 seconds
- `count=118000` on dd → stops writing after 60MB, TV freezes at 2.5 minutes
- Circular dd loop → introduces gaps when dd restarts
- Debounce/backoff → reduces server hammering but doesn't fix the read/write race
- IS_REOPEN removal → reduces false triggers but doesn't fix playback quality

**The race condition between disk reads and disk writes is unsolvable with this architecture.** No amount of timing tricks will make the TV wait for data that hasn't been written yet.

### The Solution: Eliminate Disk I/O Entirely

The FUSE sensor already intercepts every single byte the TV reads. Instead of reading from `tv_stream.img` (which has stale data), **make the FUSE sensor serve live data directly from a ring buffer in memory** that's being filled by the server's HTTP stream.

When the TV tries to read data that hasn't arrived yet, the FUSE handler **blocks** (delays the response) until the data is available. The TV's media player interprets this as a "slow USB drive" and buffers naturally — exactly like buffering a network video, but at the USB/SCSI level.

---

## 3. Proposed Architecture v2

### New Data Flow (v2 — FUSE-Direct Streaming)
```
Server (FFmpeg) ─HTTP─→ curl (on phone) ─pipe─→ ring buffer (16MB, in memory)
                                                        ↓ SYNCHRONIZED ↓
TV ─USB SCSI READ─→ f_mass_storage ─→ FUSE sensor ─→ ring_buffer_read() ─→ TV
                                          │                    (blocks if data not ready)
                                          ↓
                                    channel detection
                                    (same sector-range logic)
                                          ↓
                                    HTTP POST /api/switch
                                    + restart curl pipe
```

### What This Eliminates
| Removed Component | Why It's No Longer Needed |
|---|---|
| `dd of=tv_stream.img` | No disk writes — data goes to memory ring buffer |
| `on_channel_switch.sh` | Channel switch logic moves inside `fuse_sensor_v2.c` |
| Sector overflow bugs | No writing to disk sectors at all |
| `count=118000` limit | Ring buffer wraps automatically |
| Circular dd loop | Ring buffer is inherently circular |
| Read/write race condition | FUSE handler blocks until data is ready |
| DNS/resolve hacks | Direct HTTP to server, configured once |

### What Stays The Same
| Kept Component | Why |
|---|---|
| FAT32 image with 12 `.ts` files | TV still needs file names to browse with remote |
| `fuse_sensor` FUSE mount | Still intercepts TV reads — but now serves live data |
| `server.py` + FFmpeg hub | Still does IPTV→MPEG-TS conversion and restamping |
| Pre-injected 1080p headers | Still needed for the first ~1MB before live data arrives |
| Web remote control | Still controls channel selection from phone browser |
| ConfigFS USB gadget | Still emulates USB mass storage |

### How the Ring Buffer Works

```
Ring Buffer (16 MB = ~43 seconds at 3 Mbps)
┌─────────────────────────────────────────────────────────────┐
│ ████████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│
│ ^                           ^                               │
│ TV read position            curl write position             │
│ (FUSE handler reads here)   (background thread writes here) │
└─────────────────────────────────────────────────────────────┘

When TV read catches up to write position → FUSE handler BLOCKS
When curl writes more data → FUSE handler UNBLOCKS → TV plays
```

### Pseudocode for fuse_sensor_v2.c FUSE_READ Handler

```c
case FUSE_READ:
    offset = request->offset;
    size   = request->size;

    // 1. Find which channel this offset belongs to
    channel_idx = find_channel_by_offset(offset);

    if (channel_idx < 0) {
        // Non-channel data (FAT boot sector, directory entries, etc.)
        // Serve from backing image as before
        pread(backing_fd, buffer, size, offset);
    }
    else if (channel_idx != current_channel) {
        // CHANNEL SWITCH detected!
        current_channel = channel_idx;
        switch_channel(channels[channel_idx].id);  // POST /api/switch + restart curl
        // Serve pre-injected header from backing image (first ~1MB)
        pread(backing_fd, buffer, size, offset);
    }
    else {
        // Active channel — serve from ring buffer
        relative_offset = offset - channels[channel_idx].start_offset;

        if (relative_offset < HEADER_SIZE) {
            // Still in pre-injected header zone — serve from disk
            pread(backing_fd, buffer, size, offset);
        } else {
            // Live data zone — serve from ring buffer, BLOCKING if needed
            stream_pos = relative_offset - HEADER_SIZE;
            ring_buffer_read_blocking(buffer, size, stream_pos, timeout=5s);
        }
    }

    // Send FUSE response
    respond(buffer, size);
```

---

## 4. Complete Component Reference

### Files in `/home/sam/Code/usb-stream-tv/`

| File | Purpose | Lines | Status |
|---|---|---|---|
| [`server.py`](file:///home/sam/Code/usb-stream-tv/server.py) | Python HTTP server + FFmpeg hub + MPEG-TS restamper + web remote | ~1314 | Working, has debounce + backoff + keyframe gating |
| [`fuse_sensor.c`](file:///home/sam/Code/usb-stream-tv/fuse_sensor.c) | C FUSE handler — intercepts TV reads, detects channel by sector | 239 | v1 (disk passthrough) — needs v2 rewrite |
| [`on_channel_switch.sh`](file:///home/sam/Code/usb-stream-tv/on_channel_switch.sh) | Shell: kills old curl, POSTs /api/switch, starts curl\|dd | 76 | **To be eliminated in v2** |
| [`usb_tv.sh`](file:///home/sam/Code/usb-stream-tv/usb_tv.sh) | Main launcher: starts FUSE, configures USB gadget, initial channel | 133 | Needs minor update for v2 |
| [`run.sh`](file:///home/sam/Code/usb-stream-tv/run.sh) | Server-side: starts server.py + cloudflared tunnel, pushes to phone | 90 | Working |
| [`channel_offsets.json`](file:///home/sam/Code/usb-stream-tv/channel_offsets.json) | Sector map for all 12 channels in FAT32 image | 122 | Reference data |
| [`channels.json`](file:///home/sam/Code/usb-stream-tv/channels.json) | Full IPTV channel database (1,735 channels) with URLs | Large | Working |
| [`tv_stream_base.img`](file:///home/sam/Code/usb-stream-tv/tv_stream_base.img) | FAT32 750MB image with pre-injected H.264 1080p headers | Binary | Working |

---

## 5. The 6 Hard-Won Lessons (Do NOT Reintroduce These Bugs)

> [!CAUTION]
> Every single one of these bugs cost hours to diagnose. Read carefully.

### Bug #1 — FUSE Sensor Re-Trigger Loop (3–8 second death spiral)
- **Root cause**: The sensor had `is_reopen` logic that re-fired `on_channel_switch.sh` when the TV re-read offset 0 of the current file (Samsung ConnectShare re-probes the file header every few seconds to estimate bitrate/duration).
- **Symptom**: Stream killed and restarted every 3–8 seconds. TV showed frozen frames or black screen.
- **Fix**: Channel switch fires **only** when `current_channel != detected_channel`. Never re-trigger on the same channel. Period.

### Bug #2 — Sector Overflow (dd writes past channel boundary)
- **Root cause**: `dd` without `count=` limit wrote past the 60MB boundary of one channel into the next channel's sectors, corrupting the FAT32 structure.
- **Symptom**: After watching Globo for 3 minutes, SBT and Record showed "arquivo não suportado" because their headers were overwritten.
- **Fix**: Always use `count=118000` (or equivalent sector limit). In v2, this is moot since there are no disk writes.

### Bug #3 — "Arquivo não suportado" (Samsung probes before data arrives)
- **Root cause**: When the TV opens a `.ts` file, it reads the first 1–2 MB in < 50ms via USB. If those bytes are null/padding packets (0x47 0x1F 0xFF), the TV's format detector returns "unsupported file" before the network stream has a chance to deliver real data.
- **Fix**: Pre-inject **real** H.264 1080p + AAC MPEG-TS headers (generated with `ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=30 -t 4 -c:v libx264 -c:a aac -f mpegts test_header.ts`) into the first ~1MB of every channel's sector range. The TV reads valid video, opens the player, and by second 3 the live stream has taken over.

### Bug #4 — Timestamp Overflow (448:34:22 duration display)
- **Root cause**: The `SeamlessRestamper` in `server.py` continued PTS/PCR from the previous channel instead of resetting. When switching from Globo (which had been running for 1 hour) to SBT, the new stream started at PTS = 3,600 seconds. The TV calculated total duration as enormous.
- **Fix**: `start_new_channel()` must **always reset** PTS/PCR to `90000` (= 1.0 second in 90kHz clock). This is already implemented in current `server.py`.

### Bug #5 — HTTP 429 / 418 Rate Limiting from IPTV Provider
- **Root cause**: The IPTV provider enforces `max_connections: 1`. When FFmpeg crashed and the `_worker` loop restarted it instantly, the provider's Cloudflare proxy blocked with 429/418 errors. The rapid retry loop made it worse.
- **Fix**: Exponential backoff in `_worker` (2s → 4s → 8s → ... → 30s max) when FFmpeg exits unexpectedly. Reset backoff on intentional channel switch. Also server-side debounce: ignore duplicate `/api/switch` for same channel within 3 seconds.

### Bug #6 — Unnecessary Cloudflare Tunnel Round-Trip
- **Root cause**: Even when the phone and PC are on the same Wi-Fi (192.168.1.x), the stream routed through: phone → internet → Cloudflare → internet → PC → internet → Cloudflare → internet → phone. Four unnecessary network hops.
- **Fix**: `on_channel_switch.sh` (and in v2, the FUSE sensor) tries `http://SERVER_LAN_IP:8080` first with a 1-second timeout, falls back to tunnel URL only if LAN is unreachable.

---

## 6. IPTV Credentials & API Reference

Extracted from `/home/sam/Code/usb-stream-tv/extracted_iptv/PREF.xml`:

| Field | Value |
|---|---|
| **Server** | `http://xc.sspkdns.com` |
| **Port** | `80` |
| **Username** | `819818622153` |
| **Password** | `255784120272` |

### Xtream Codes URL Patterns

```bash
# Live channel stream
http://xc.sspkdns.com/live/819818622153/255784120272/{STREAM_ID}.ts

# Movie (VOD)
http://xc.sspkdns.com/movie/819818622153/255784120272/{STREAM_ID}.mp4

# Series episode
http://xc.sspkdns.com/series/819818622153/255784120272/{STREAM_ID}.mp4

# Full M3U playlist
http://xc.sspkdns.com/get.php?username=819818622153&password=255784120272&type=m3u_plus&output=ts

# JSON API endpoints
http://xc.sspkdns.com/player_api.php?username=819818622153&password=255784120272&action=get_live_categories
http://xc.sspkdns.com/player_api.php?username=819818622153&password=255784120272&action=get_live_streams
http://xc.sspkdns.com/player_api.php?username=819818622153&password=255784120272&action=get_vod_categories
http://xc.sspkdns.com/player_api.php?username=819818622153&password=255784120272&action=get_vod_streams
```

> [!IMPORTANT]
> The IPTV provider enforces **max_connections: 1**. Only one FFmpeg instance can be connected at a time. If two connect simultaneously, the provider returns HTTP 429 or 418 and may temporarily block the account.
> The **User-Agent** must be `IPTVSmartersPro` or the provider rejects the request.

---

## 7. Current Channel Sector Map (FAT32 Layout)

These are the byte offsets within `tv_stream.img` (750MB FAT32 image) where each channel's `.ts` file data begins and ends. These offsets are determined by the FAT32 cluster allocation when the image was created.

| # | Channel Name (FAT32 File) | Channel ID | Start Sector | End Sector | Start Byte Offset |
|---|---|---|---|---|---|
| 01 | 01 - TV GAZETA SP 1080p.ts | `tv-gazeta-sp` | 3,072 | 121,848 | 1,572,864 |
| 02 | 02 - SBT NEWS 720p.ts | `sbt-news` | 121,848 | 240,624 | 62,386,176 |
| 03 | 03 - RECORD NEWS 1080p.ts | `record-news` | 240,624 | 359,400 | 123,199,488 |
| 04 | 04 - REDETV NACIONAL 720p.ts | `rede-tv-nacional` | 359,400 | 478,176 | 184,012,800 |
| 05 | 05 - TV CULTURA SP 720p.ts | `tv-cultura-sp` | 478,176 | 596,952 | 244,826,112 |
| 06 | 06 - TV BRASIL EBC 720p.ts | `tv-brasil-ebc` | 596,952 | 715,728 | 305,639,424 |
| 07 | 07 - TV SENADO 480p.ts | `tv-senado` | 715,728 | 834,504 | 366,452,736 |
| 08 | 08 - CANAL FUTURA 720p.ts | `canal-futura` | 834,504 | 953,280 | 427,266,048 |
| 09 | 09 - REDE BRASIL 1080p.ts | `rede-brasil` | 953,280 | 1,072,056 | 488,079,360 |
| 10 | 10 - TV APARECIDA 720p.ts | `tv-aparecida` | 1,072,056 | 1,190,832 | 548,892,672 |
| 11 | 11 - TV JUSTICA 720p.ts | `tv-justica` | 1,190,832 | 1,309,608 | 609,705,984 |
| 12 | 12 - REDE GLOBO 720p.ts | `globo-morena-dourados` | 1,309,608 | 1,428,384 | 670,519,296 |

Each channel occupies ~60MB (118,776 sectors × 512 bytes/sector).

> [!NOTE]
> If you recreate the FAT32 image with different files/names, these offsets will be completely different. You must re-run the cluster mapper to get the new sector ranges and update `fuse_sensor.c` accordingly.

---

## 8. Hardware Reference

### TV
- **Model**: Samsung Plasma PL51F4000 (~2013)
- **USB Port**: ConnectShare (USB 2.0 Type-A, 500mA power)
- **Supported Codecs**: H.264 Main/High Profile up to 1080p, AAC, AC3. Does NOT support HEVC/H.265, EAC3, DTS.
- **Filesystem**: FAT32 only (max 4GB per file). No NTFS, no exFAT.
- **Container**: MPEG-TS (`.ts`) works best. No MP4 moov atom issues.
- **Quirk**: Re-reads file offset 0 periodically while playing (for bitrate estimation / seekbar). This is what triggered Bug #1.

### Previous Android Device (Xiaomi Mi A2 — being replaced)
- **Codename**: `jasmine_sprout`, Serial `4773620`
- **Root**: Magisk
- **Kernel**: Supports `/sys/kernel/config/usb_gadget/g1` (ConfigFS)
- **CPU**: ARM64 (aarch64) — binary compiled with `aarch64-linux-gnu-gcc -static`
- **Termux packages needed**: `tsu curl ffmpeg util-linux dosfstools`

### New Device (TBD — tablet/phone spare)
Before using any new device, verify:
```bash
su
# 1. Check USB Gadget ConfigFS exists
ls -d /sys/kernel/config/usb_gadget/g1 2>/dev/null && echo "OK: ConfigFS"

# 2. Check mass_storage function available
grep -i mass_storage /proc/kallsyms | head -3

# 3. Check CPU architecture (for cross-compilation)
uname -m   # expect aarch64 or armv7l

# 4. Check /dev/fuse exists (for FUSE sensor)
ls -la /dev/fuse
```

---

## 9. Setup From Scratch Checklist

### On the Server (PC or VPS)

1. **Install dependencies**:
   ```bash
   sudo apt install python3 ffmpeg
   # For cross-compilation of fuse_sensor:
   sudo apt install gcc-aarch64-linux-gnu  # for ARM64 devices
   # or: sudo apt install gcc-arm-linux-gnueabihf  # for ARM32 (Pi Zero)
   ```

2. **Clone or copy project** to `/home/sam/Code/usb-stream-tv/` (or `/opt/usb-stream-tv/` on VPS).

3. **Download channels** (if channels.json needs refresh):
   ```bash
   curl -s "http://xc.sspkdns.com/player_api.php?username=819818622153&password=255784120272&action=get_live_streams" > channels_raw.json
   ```

4. **Compile fuse_sensor** for target device:
   ```bash
   aarch64-linux-gnu-gcc -static -O2 -lpthread fuse_sensor.c -o fuse_sensor
   ```

5. **Generate FAT32 image** (if recreating):
   ```bash
   # Create 750MB (or larger) sparse image
   truncate -s 750M tv_stream_base.img
   mkfs.fat -F 32 -n LIVETV tv_stream_base.img

   # Create folders and placeholder files using mtools
   mmd -i tv_stream_base.img "::01 - CANAIS ABERTOS"
   mmd -i tv_stream_base.img "::02 - ESPORTES"
   mmd -i tv_stream_base.img "::03 - FILMES E SERIES"

   # Create channel .ts files inside folders (each ~60MB)
   dd if=/dev/zero bs=1M count=58 | mcopy -i tv_stream_base.img - "::01 - CANAIS ABERTOS/01 - GLOBO.ts"
   # ... repeat for all channels

   # Map cluster offsets
   # Use: mdir -i tv_stream_base.img to find start clusters
   # Then: python3 map_clusters.py to calculate sector ranges
   ```

6. **Inject valid headers** into every channel's start offset:
   ```bash
   # Generate a 4-second 1080p H.264+AAC test clip
   ffmpeg -f lavfi -i "testsrc=size=1920x1080:rate=30" \
          -f lavfi -i "sine=frequency=440:sample_rate=44100" \
          -t 4 -c:v libx264 -preset ultrafast -c:a aac \
          -f mpegts test_header.ts

   # Inject into each channel's start sector
   dd if=test_header.ts of=tv_stream_base.img seek=3072 bs=512 conv=notrunc    # Globo
   dd if=test_header.ts of=tv_stream_base.img seek=121848 bs=512 conv=notrunc  # SBT
   # ... repeat for all 12 channels
   ```

7. **Start server**:
   ```bash
   ./run.sh  # Starts server.py + cloudflared tunnel + pushes to phone via ADB
   ```

### On the Android Device

1. **Install Termux** (F-Droid or GitHub releases).
2. **Install packages**: `pkg update && pkg install tsu curl`
3. **Push files via ADB** (from PC):
   ```bash
   adb push fuse_sensor /data/local/tmp/
   adb push usb_tv.sh /data/local/tmp/
   adb push on_channel_switch.sh /data/local/tmp/  # only needed in v1
   adb push tv_stream_base.img /data/local/tmp/tv_stream.img
   adb shell "su -c 'chmod +x /data/local/tmp/fuse_sensor /data/local/tmp/usb_tv.sh /data/local/tmp/on_channel_switch.sh'"
   ```
4. **Start**:
   ```bash
   su
   cd /data/local/tmp
   ./usb_tv.sh start
   ```
5. **Plug USB cable** from phone to TV.
6. On TV: Menu → USB (LIVETV) → Videos → Select channel.

### For VPS Deployment
```bash
# On VPS:
sudo apt install python3 ffmpeg nginx certbot python3-certbot-nginx

# Copy server.py and channels.json to /opt/usb-stream-tv/
# Create systemd service:
cat > /etc/systemd/system/usb-tv.service << 'EOF'
[Unit]
Description=USB Stream TV Server
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/usb-stream-tv
ExecStart=/usr/bin/python3 /opt/usb-stream-tv/server.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now usb-tv

# Nginx reverse proxy:
cat > /etc/nginx/sites-available/tv << 'EOF'
server {
    server_name tv.yourdomain.com;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_buffering off;
        proxy_read_timeout 3600s;
    }
}
EOF
sudo ln -s /etc/nginx/sites-available/tv /etc/nginx/sites-enabled/
sudo certbot --nginx -d tv.yourdomain.com
```

Then on the phone, set `SERVER_URL="https://tv.yourdomain.com"` — no Cloudflare tunnel needed.

---

## 10. Movies / VOD Feature (Not Yet Implemented)

The user requested the ability to browse and play movies from the web remote:

### Design
1. Add a **"🎬 Cinema"** tab to the web remote in `server.py`.
2. Fetch movie catalog from Xtream Codes API (`action=get_vod_streams`).
3. When user clicks a movie, POST to `/api/switch` with custom URL:
   ```json
   {"custom_url": "http://xc.sspkdns.com/movie/819818622153/255784120272/12345.mp4", "custom_name": "Movie Title"}
   ```
4. FFmpeg handles the transmux from MP4/MKV → MPEG-TS automatically.
5. On the TV, the user opens a dedicated `CINEMA.ts` file to watch whatever movie was selected.

---

## 11. Diagnostic Commands

### On the Phone
```bash
# Check if FUSE sensor is running
pgrep -fa fuse_sensor

# Watch channel detections in real-time
tail -f /data/local/tmp/sensor.log

# Check streaming process
pgrep -fa "curl.*live"

# Check USB gadget status
cat /sys/kernel/config/usb_gadget/g1/UDC
cat /sys/kernel/config/usb_gadget/g1/functions/mass_storage.0/lun.0/file

# Full status
./usb_tv.sh status
```

### On the Server
```bash
# Server health
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool

# Force channel switch
curl -X POST http://127.0.0.1:8080/api/switch \
     -H "Content-Type: application/json" \
     -d '{"channel_id": "1000000061"}'

# Test IPTV connectivity (must use correct User-Agent)
curl -s -A "IPTVSmartersPro" --max-time 5 \
     "http://xc.sspkdns.com/live/819818622153/255784120272/1000000061.ts" \
     -o /dev/null -w "HTTP %{http_code}, %{size_download} bytes in %{time_total}s\n"

# Web remote
# Open in browser: http://127.0.0.1:8080/ (or tunnel URL)
```

---

## 12. Key Server.py Internals (For the Next Developer)

### SeamlessRestamper (lines ~125–247)
Normalizes MPEG-TS timestamps so the TV's hardware decoder doesn't choke on discontinuities:
- Resets PTS/PCR to `90000` (1 second) on every channel switch
- Maintains continuity counters (CC) per PID
- **Keyframe gating**: Replaces non-IDR video packets with null packets until first SPS/PPS/IDR is seen — prevents TV from trying to decode a partial I-frame

### StreamHub (lines ~249–415)
- Manages one FFmpeg subprocess per active channel
- `_worker()` loop: runs FFmpeg → reads 65,424 bytes (188 × 348 packets) → restamps → distributes to subscriber queues
- `switch_channel()`: debounce 3s for same channel, flushes subscriber queues, terminates old FFmpeg, signals `switch_event`
- Exponential backoff on FFmpeg failure (2s → 4s → 8s → ... → 30s)

### HTTP Endpoints
- `GET /` → Web remote control UI (full HTML/JS/CSS embedded in server.py)
- `GET /live.ts` → Chunked MPEG-TS stream (subscribe to StreamHub queue)
- `GET /api/status` → JSON: active channel, listeners, uptime
- `POST /api/switch` → Change channel by ID or custom URL
- `GET /api/channels` → Channel list as JSON

### FFmpeg Command Used
```python
cmd = [
    "ffmpeg", "-hide_banner", "-loglevel", "warning",
    "-user_agent", "IPTVSmartersPro",
    "-reconnect", "1", "-reconnect_streamed", "1",
    "-reconnect_delay_max", "3", "-reconnect_at_eof", "1",
    "-probesize", "500000", "-analyzeduration", "1000000",
    "-i", url,
    "-c:v", "copy",                                    # Video: passthrough (no transcode)
    "-c:a", "aac", "-b:a", "128k", "-ar", "44100", "-ac", "2",  # Audio: force AAC stereo
    "-avoid_negative_ts", "make_zero",
    "-fflags", "+genpts+discardcorrupt+nobuffer",
    "-flags", "low_delay",
    "-muxdelay", "0", "-muxpreload", "0",
    "-f", "mpegts", "pipe:1"
]
```

---

## 13. Network Configuration

| Component | Current IP | Port |
|---|---|---|
| PC (server) | `192.168.1.8` | `8080` |
| Phone (client) | DHCP (192.168.1.x) | N/A |
| Cloudflare Tunnel | Changes each restart | 443 |

The `on_channel_switch.sh` (v1) tries LAN first (`http://192.168.1.8:8080`), falls back to tunnel URL from `/data/local/tmp/server_url.txt`.

For v2 (FUSE-direct), the server URL is compiled into `fuse_sensor_v2.c` or read from a config file at startup.
