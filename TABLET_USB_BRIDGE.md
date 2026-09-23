# Tablet USB Bridge — Samsung Galaxy Tab 3 Lite (SM-T110)

> **Status: Working ✅** — Live MPEG-TS stream confirmed playing on Samsung Plasma TV via ConnectShare.

---

## 1. Hardware Overview

| Property | Value |
|----------|-------|
| **Model** | Samsung Galaxy Tab 3 Lite 7.0 (SM-T110) |
| **Android codename** | `goyawifixx` |
| **Serial** | `300451dc03fe8100` |
| **SoC** | Marvell PXA986 (Dual-Core Cortex-A9) |
| **ABI** | `armeabi-v7a` (ARM32, hard-float) |
| **Android version** | 4.2.2 Jelly Bean (API 17) |
| **Kernel** | Linux 3.4.5 |
| **Root method** | TWRP 2.7.0.1 + SuperSU v2.76 (flashable ZIP) |
| **SELinux** | Disabled |

---

## 2. Full Stack Architecture

```
[Oracle VPS — tv.smre.run.place]
  server.py → GET /stream → raw MPEG-TS (HTTP/1.1, chunked)
       │
       │  (HTTP TCP socket over chisel reverse tunnel or direct WiFi)
       ▼
[Tablet — SM-T110]
  stream_fetcher (ARM32 static C binary)
       │  writes raw TS bytes (blocking)
       ▼
  /data/local/tmp/live_pipe  (named FIFO)
       │  reads bytes continuously (feeder thread)
       ▼
  fuse_direct (ARM32 static C binary)
  ┌─────────────────────────────────────────┐
  │  32 MB RAM ring buffer                  │
  │  FAT32 virtual filesystem (4 GB disk)   │
  │  Single file: "TV AO VIVO.ts"          │
  │  Exposed via /dev/fuse                  │
  └─────────────────────────────────────────┘
       │  FUSE mount at /data/local/tmp/vfat_mnt/tv_stream.img
       ▼
  Android USB Gadget — f_mass_storage (legacy android_usb)
  lun0 → tv_stream.img (read-only)
  functions: mass_storage,adb  (ADB stays alive!)
       │  USB-A to USB-B cable
       ▼
[Samsung Plasma TV]
  ConnectShare → reads "TV AO VIVO.ts" → plays live MPEG-TS ✅
```

---

## 3. Binaries Installed on Tablet (`/system/xbin/`)

| Binary | Source | Role |
|--------|--------|------|
| `fuse_direct` | `fuse_direct.c` (ARM32 static) | FUSE server — virtual FAT32 disk fed by FIFO |
| `stream_fetcher` | `stream_fetcher.c` (ARM32 static) | HTTP → FIFO forwarder with auto-reconnect |
| `chisel` | jpillora/chisel v1.12.0 armv7 | Reverse TCP tunnel client (WiFi bypass) |
| `busybox` | busybox-armv7l 1.31.0 static musl | `mkfifo`, `wget`, `sh`, 300+ utils |
| `su` | SuperSU v2.76 | Root escalation |
| `daemonsu` | SuperSU v2.76 | Root daemon (started at boot) |
| `start_tv.sh` | `start_tv_tablet.sh` (this repo) | Launches entire stack |
| `stop_tv.sh` | `stop_tv_tablet.sh` (this repo) | Stops all services |
| `status_tv.sh` | `status_tv_tablet.sh` (this repo) | Shows process/gadget/log state |

### System Files on Tablet

| Path | Purpose |
|------|---------|
| `/system/etc/fat_template.bin` | 11-sector FAT32 geometry template (from `gen_template.py`) |
| `/system/etc/install-recovery.sh` | SuperSU boot hook (calls `install-recovery-2.sh`) |
| `/system/etc/install-recovery-2.sh` | Auto-start hook: `sleep 15 && start_tv.sh` |
| `/system/etc/resolv.conf` | `nameserver 8.8.8.8` (DNS for busybox wget) |
| `/etc/hosts` | `129.146.5.64 tv.smre.run.place` (DNS bypass if needed) |

---

## 4. Key Configuration Constants

### `fuse_direct.c`

```c
#define FILE_SIZE  1800000000ULL  // 1.8 GB virtual file (~96 min @ 305 KB/s)
#define RINGSZ     (32ULL * 1024 * 1024)  // 32 MB RAM ring buffer (~90s @ 2.9 Mbps)
#define LEADBACK   (12ULL * 1024 * 1024)  // TV reads ~40s behind live edge
#define TOTCLUS    1048576ULL              // 4 GB virtual disk total
#define MNT_POINT  "/data/local/tmp/vfat_mnt"
#define FILE_NAME  "TV AO VIVO.ts"        // Filename shown to TV
#define DEF_FIFO   "/data/local/tmp/live_pipe"
#define DEF_TMPL   "/data/local/tmp/fat_template.bin"
```

### USB Gadget (Legacy `android_usb` sysfs)

```
/sys/class/android_usb/android0/functions     → "mass_storage,adb"
/sys/class/android_usb/android0/f_mass_storage/lun0/file → .../tv_stream.img
/sys/class/android_usb/android0/f_mass_storage/lun0/ro   → 1  (read-only!)
```

> **Important**: `lun0/ro=1` must be written **before** `lun0/file`. If the TV sees
> a writable disk it will attempt to write filesystem metadata and corrupt the stream.

### Chisel Reverse Tunnel

| Parameter | Value |
|-----------|-------|
| Server URL | `http://tv.smre.run.place/chisel` |
| Forwarded tunnel | VPS `:25555` → Tablet `127.0.0.1:5555` (adbd) |
| Auth | `tablet:tvbridge2026` |
| Keepalive | 15s |

---

## 5. Operating the Tablet Bridge

### Start

```bash
# On tablet (as root) — or happens automatically at boot
su -c /system/xbin/start_tv.sh
```

### Stop

```bash
su -c /system/xbin/stop_tv.sh
```

### Status

```bash
su -c /system/xbin/status_tv.sh
```

### Plug into TV

1. Run `start_tv.sh` (or it auto-started at boot)
2. Connect USB cable: Tablet → TV (USB-A port, "ConnectShare")
3. TV remote: **Source → ConnectShare** → navigate to `TV AO VIVO.ts` → Play ✅

---

## 6. Remote Management via Oracle VPS

### The Problem: AP Isolation

The tablet and PC are on the same WiFi network, but the router has **AP isolation** enabled — devices cannot communicate directly with each other. ADB over LAN is blocked.

### The Solution: Chisel Reverse Tunnel

The tablet's `chisel` client connects **outbound** to the Oracle VPS (bypasses AP isolation). The VPS then exposes port `25555` which tunnels back to the tablet's ADB port (`5555`).

```
PC  ─────SSH tunnel──────►  Oracle VPS :25555
                                   │  chisel reverse tunnel
                                   ▼
                            Tablet :5555 (adbd)
```

### Quick Connect (from PC)

```bash
# Uses connect_tablet.sh helper script:
./connect_tablet.sh             # Opens root ADB shell
./connect_tablet.sh id          # Run: su -c 'id'
./connect_tablet.sh start_tv.sh # Run: su -c 'start_tv.sh'
```

### Manual Steps

```bash
# 1. Forward local port to Oracle VPS
ssh -f -N -L 25555:127.0.0.1:25555 oracle

# 2. Connect ADB via tunnel
adb connect 127.0.0.1:25555

# 3. Root shell
adb -s 127.0.0.1:25555 shell "su"

# 4. Run any command as root
adb -s 127.0.0.1:25555 shell "su -c 'status_tv.sh'"
```

---

## 7. Boot Auto-Start Mechanism

```
[Android Boot]
    │
    ▼
daemonsu starts (SuperSU daemon, via /system/etc/install-recovery.sh)
    │
    ▼
/system/etc/install-recovery.sh  →  calls install-recovery-2.sh
    │
    ▼
/system/etc/install-recovery-2.sh:
    sleep 15  # wait for WiFi to connect
    /system/xbin/start_tv.sh  &
    │
    ▼
All four services launch:
  - fuse_direct  (FUSE virtual disk)
  - chisel       (Reverse tunnel → VPS with SOCKS5)
  - stream_fetcher (HTTP → FIFO)
  - tv_watchdog  (USB gadget & process watchdog)
```

After 15–20 seconds from power-on:
- Tablet is reachable via `./connect_tablet.sh`
- USB gadget serving `tv_stream.img` (plug cable into TV)
- Watchdog enforcing `mass_storage,adb` and screen backlight at 0
- Stream playing live TV


---

## 8. Critical Implementation Notes

### FUSE Kernel Compatibility (Linux 3.4)

The tablet runs Linux 3.4.5. The FUSE protocol uses minor version negotiation. For kernels with `minor < 23`, the `FUSE_INIT` response must be exactly **24 bytes** (`FUSE_COMPAT_22_INIT_OUT_SIZE`), not the modern 64-byte `fuse_init_out`. Sending the wrong size causes the kernel to reject all subsequent FUSE operations with `EPROTO`.

Fix applied in `fuse_direct.c` — the init response size is conditionally selected based on the kernel's negotiated minor version.

### Shell Interpreter for Scripts

Tablet scripts use `#!/system/xbin/busybox sh` — **not** `#!/system/bin/sh`. The stock MKS shell on Android 4.2.2 lacks support for heredocs, background process management, and several built-in commands needed by the start script.

### ADB Kept Alive During Streaming

`functions = mass_storage,adb` keeps ADB active even while mass storage is exposed. This is essential for remote management. If `functions` must be changed, the sequence is: `enable=0` → change `functions` → `enable=1`.

### stream_fetcher DNS Fallback

`stream_fetcher.c` uses `gethostbyname()` for DNS resolution and falls back to hardcoded IP `129.146.5.64` if DNS fails. This ensures the stream reconnects even if DNS is unavailable.

---

## 9. Oracle VPS Infrastructure (for reference)

| Component | Details |
|-----------|---------|
| **Hostname** | `oracle` (SSH alias) |
| **IP** | `129.146.5.64` |
| **Domain** | `tv.smre.run.place` |
| **Architecture** | ARM64, Docker |
| **chisel server** | systemd: `/etc/systemd/system/chisel.service`, port `8888` |
| **Caddy** | `/opt/containers/core/data/caddy/conf.d/usb-stream-tv.caddy` |
| **Stream server** | `usb-stream-tv` Docker container, port `8080` |
| **Stream URL** | `http://tv.smre.run.place/stream` |
| **Chisel WebSocket** | `http://tv.smre.run.place/chisel` → Caddy → host `172.20.0.1:8888` |

---

## 10. Rooting Process (SM-T110 Reference)

The standard one-click root APKs (KingoRoot, Framaroot, Towelroot) **do not support** the Marvell PXA986 SoC. The only working path was:

1. Download `T110.TWRP.2.7.0.1.tar.md5` (in this repo)
2. Boot into Download Mode: `Power + Volume Down + Home` (hold)
3. Flash TWRP using `odin4` (Linux): `odin4 -a T110.TWRP.2.7.0.1.tar.md5`
4. Boot into Recovery: `Power + Volume Up + Home` (hold)
5. In TWRP shell: mount `/system` RW, flash `supersu_flashable.zip`
6. Disable stock recovery restoration: rename `install-recovery.sh` and `recovery-from-boot.p`

> **All rooting tools and the TWRP image are preserved in this repository.**

---

## 11. Reverse SOCKS5 & HTTP Residential Proxy (VOD CDN Bypass)

### The Problem
Upstream VOD streams (`/movie/`, `/series/`, and CDNs such as `fontedecanais`) enforce strict Cloudflare and datacenter IP blocks. HTTP GET requests from Oracle Cloud VPS IPs (`129.146.5.64`) return `403 Forbidden`. Requests from residential IPs return `200 OK`.

### The Solution: Double-Tied Proxy
1. **Chisel Reverse SOCKS5**:
   The tablet's Chisel client passes `R:0.0.0.0:1080:socks` alongside the ADB tunnel:
   ```bash
   /system/xbin/chisel client --keepalive 15s --auth tablet:tvbridge2026 http://tv.smre.run.place/chisel R:25555:127.0.0.1:5555 R:0.0.0.0:1080:socks
   ```
   The VPS Chisel server listens on port `1080` (accessible to the Docker bridge network `172.20.0.1:1080`). All traffic routed through port `1080` exits through the tablet's residential Wi-Fi (`177.131.189.142`).
2. **Privoxy HTTP-to-SOCKS5 Bridge**:
   FFmpeg's native HTTP protocol only supports HTTP proxies (not SOCKS proxies) for byte-range requests. Privoxy is installed on the VPS host listening on `172.20.0.1:8118` forwarding to `127.0.0.1:1080`.
   - `build_ffmpeg_cmd` in `server.py` detects VOD streams (`.mp4`, `.mkv`, `fontedecanais`, `movie`, `series`, YouTube) and passes `-http_proxy http://172.20.0.1:8118`.
   - Direct SOCKS5 (`172.20.0.1:1080`) remains available for `yt-dlp` for future YouTube streaming.

---

## 12. USB Gadget Watchdog, Disconnect Recovery & Power Management

### MTP Fallback Bug & Root Cause
Whenever the TV was power-cycled or the USB cable was momentarily replugged, the Android kernel detected a 5V VBUS drop. Samsung's `UsbDeviceManager` automatically reverted `sys.usb.config` to the default system setting (`persist.sys.usb.config = mtp,adb`).
Because the tablet identified itself as an MTP media player, the Samsung Plasma TV (which strictly supports USB Mass Storage SCSI drives) reported *"No USB device connected"*.

### The Fix
1. **System Property Enactment**:
   Set `persist.sys.usb.config=mass_storage,adb` and `sys.usb.config=mass_storage,adb`.
2. **Watchdog Daemon (`tv_watchdog.sh`)**:
   Runs as a background daemon on the tablet every 2 seconds:
   - Enforces `functions` contains `mass_storage`.
   - Re-anchors `f_mass_storage/lun0/file` to `/data/local/tmp/vfat_mnt/tv_stream.img`.
   - Enforces `f_mass_storage/lun0/ro = 1` before enabling the gadget.
   - Monitors `stream_fetcher` and `chisel` processes.
3. **Power & Backlight Optimization**:
   The Samsung Plasma TV USB port provides 500mA max. The tablet's screen stay-awake was drawing excessive power, dropping battery voltage to `3603 mV`.
   The watchdog keeps panel brightness at `0` (`/sys/class/backlight/panel/brightness`), allowing the tablet to steadily charge at `3850 mV` from the TV USB port.


---

## 13. ADB Hardening — Persistent TCP Port 5555

### Root Cause of ADB Loss
A critical failure mode was discovered: when `sys.usb.config` was set to `mass_storage` (without `,adb`), Android `init` immediately executed `stop adbd`, closing TCP port 5555. This silently killed the Chisel reverse tunnel (`127.0.0.1:25555`) and made the tablet completely unreachable over WiFi.

### The Fix
1. **Enforce composite gadget**: Always use `mass_storage,adb` (never `mass_storage` alone). Samsung ConnectShare accepts composite USB gadgets without issue.
2. **Lock persist property**: `setprop persist.adb.tcp.port 5555` — ensures `adbd` always binds TCP 5555 on restart.
3. **Watchdog self-healing ADB**: `tv_watchdog.sh` checks every 2s:
   ```bash
   if ! netstat -tlpn 2>/dev/null | grep -q ":5555 "; then
       setprop service.adb.tcp.port 5555
       stop adbd; start adbd
   fi
   ```
4. **One-shot fix script**: `fix_tablet_adb.sh` (run from dev machine when tablet is connected via USB) — detects TWRP or normal boot, pushes updated scripts, locks persist props, verifies tunnel at `127.0.0.1:25555`.

### Verified State
```
persist.adb.tcp.port=5555
persist.sys.usb.config=mass_storage,adb
netstat: 0.0.0.0:5555 LISTEN
127.0.0.1:25555 device   ← confirmed via chisel tunnel
```

---

## 14. HTTP Fallback Remote Command Channel

### Problem
The Chisel tunnel and ADB over TCP (`127.0.0.1:25555`) are the primary remote management path. If ADB ever drops (see Section 13), there was previously no secondary path to reach the tablet for diagnostics or recovery.

### Solution: Polling HTTP Channel
The tablet's `tv_watchdog.sh` polls the VPS server every **30 seconds**:
```bash
CMD=$(busybox wget -qO- http://tv.smre.run.place/api/tablet_cmd)
if [ "$CMD" != "none" ]; then
    RESULT=$(eval "$CMD" 2>&1)
    busybox wget -qO- --post-data="result=$RESULT" http://tv.smre.run.place/api/tablet_cmd_res
fi
```

### Server API Endpoints (added to `server.py`)
| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/tablet_cmd` | GET | Returns pending command for tablet (or `"none"`) |
| `/api/tablet_cmd_res` | POST | Tablet posts command result back (body: `result=...`) |
| `/api/tablet_exec` | POST | Dev machine sends command; waits up to 10s for tablet response |

### Usage from Dev Machine
```bash
# Send command to tablet (waits up to 10s for response from watchdog poll)
curl -s -X POST -H "Content-Type: application/json" -H "X-Auth-PIN: 1233" \
  -d '{"cmd": "uptime"}' https://tv.smre.run.place/api/tablet_exec

# Expected response (up to 10s latency due to 30s polling interval):
# {"output": "up 2 days, 3:14, load average: 0.00 0.00 0.00"}
```

> **Latency**: Up to 30 seconds (poll interval). This is an emergency fallback, not a real-time shell.

---

> **Last updated**: 2026-09-23 by Antigravity agent. Sections 13 & 14 document ADB hardening and HTTP fallback channel added in this session.
