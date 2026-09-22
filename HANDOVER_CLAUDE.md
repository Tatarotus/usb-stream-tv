# USB Stream TV — Handover for Next Agent (Claude Opus)

> **ROLE DEFINITION — read first.**
> You (Claude Opus) are the **SUPERVISOR**, not the implementer.
> An OpenCode coding agent is the **EXECUTOR**: it runs all commands, edits
> files, operates ADB, and reports raw results back to the user.
> You never touch the machine. You review evidence, challenge measurements,
> spot design flaws the executor is too close to see, and propose
> breakthroughs. The user relays between you and the executor.
>
> **How to write back:** append dated entries under `## 7. Supervision Log`
> below (keep each entry short: verdict → evidence cited → concrete next
> experiment). The executor reads this file before every work block.
> Wanted from you: measurement critiques ("that metric can't prove X"),
> cheaper decisive experiments, architectural simplifications, and
> identification of which current complexity is load-bearing vs accidental.
> Not wanted: re-deriving what is already proven in §4, or instructions the
> executor already follows in §5.

Date: 2026-09-20 ~20:50 UTC-3. Everything below verified by execution, not assumption.

## 1. What this is
Live TV on a Samsung Plasma PL51F4000 (USB ConnectShare only, no network).
A rooted Xiaomi Mi A2 (`jasmine_sprout`, adb serial `4773620`) emulates a USB
flash drive (`LIVETV` / `CANAL AO VIVO.ts`, single-file mode). A Python writer
on the phone pulls `live.ts` from `server.py` on this PC and writes it
direct-to-sectors into the FAT32 image. TV plays the file; user zaps channels
via web remote. Repo: `/home/sam/Code/usb-stream-tv/`.

## 2. Current live state (do not assume — re-verify on start)
- `server.py` on this PC (`0.0.0.0:8080`), 41 channels, transcodes everything
  to H.264 720p30 + AC3 via ffmpeg (`build_ffmpeg_cmd`, server.py:204-246).
  Started 19:08, stable for 90+ min. Check: `curl localhost:8080/api/status`.
- Cloudflare tunnel (this PC): URL changes every restart! Current:
  `https://academy-sometimes-stands-muscles.trycloudflare.com`
  (`tunnel_url.txt`). Web remote `/`, stream `/live.ts`, `/api/status`,
  `/api/switch`, `/api/channels`. Process: `bin/cloudflared`.
- Phone writer: `stream_writer.py` (PID varies), sector 3112
  (offset 1593344, cluster 3 of single-file img), 60 MB circular window,
  tunnel URL. OOM-protected (`oom_score_adj=-900`).
  Watchdog `watch_writer.sh` restarts it ≤15s on death (kill-tested).
  Logs: `/data/local/tmp/channel_stream.log`. Heartbeat with sector pos:
  `/data/local/tmp/writer_pos.txt` (`<offset> <total> <channel> <HH:MM:SS>`).
- Phone scripts (all in `/data/local/tmp/`): `stream_writer.py`,
  `on_channel_switch.sh` (python LAN probe, persists `current_channel.txt`),
  `watch_writer.sh`, `usb_tv.sh`, `prefill_head.py`, `rebind_lun.sh`.
  Termux home has `usb_tv.sh` copy. Termux python:
  `/data/data/com.termux/files/usr/bin/python3`. Phone has NO curl.
- USB gadget: `f1=mass_storage(tv_stream.img)` + `f2=adb`, UDC `a800000.dwc3`.
  **Android's USB HAL wipes this on every cable replug** (reverts to MTP+ADB,
  clears LUN file → 0B "media removed"). Procedure: plug cable FIRST, then
  re-apply storage from Termux root (see `rebind_lun.sh` + relink f1).
- Phone on this PC appears as `/dev/sdb` → auto-mounted at
  `/run/media/sam/LIVETV/CANAL AO VIVO.ts` (vfat rw — READ ONLY from PC).
- Network topology that matters: AP isolates Wi-Fi clients (phone
  192.168.1.4 cannot reach PC .5/.8). Phone→server goes via
  `adb reverse tcp:8080` (USB, primary) or cloudflared tunnel (TV-time,
  fallback). `on_channel_switch.sh` probes `127.0.0.1 → .5 → .8 → tunnel`.

## 3. Main goal
A TV viewer opening `CANAL AO VIVO.ts` always gets fresh, playable video
(head age < ~3 min), survives writer death (watchdog), and follows web-remote
channel switches within ~1 min — proven on the REAL file, not the server
stream. Then: same on the actual Samsung TV.

## 4. Progress so far (all measured)
- `channels.json` 26 → 41 (open + premium) via `sync_iptv.py`; upstream
  playlists 41/41 reachable.
- Live decode 218–300/240 frames per 8–10s across globo/record/sony/axn/
  espn/sbt (open + premium + sport).
- TS integrity: 100% `0x47` sync (phase-adjusted reads!), CC 0 discontinuities
  on real PIDs, PAT/PMT/video-256/audio-257 present, SPS/PPS/IDR periodic.
- Restamper exonerated (single-shot + 65KB-incremental transcodes decode
  0-error); `non-existing PPS` warnings are mid-stream-join artifacts.
- Visual proofs: Globo electoral ad (fresh), Sony logo bug post-switch,
  Record/Aldeia News, TV UFOP frames — all extracted from phone sectors.
- Bugs fixed: stale tunnel URL, no-curl LAN probe, multi-LAN candidates,
  pkill self-suicide (bracket patterns), LMK kills (oom + watchdog),
  409MB→60MB window (head age 17min → ~2min), gadget/LUN re-apply procedure.
- Repo synced to phone-best versions (`stream_writer.py` with pre-switch POST).

## 5. CLOSED LOOP TEST (must run on the real file)
An 8-hop round-trip identity loop is the acceptance test
(`identity_loop.py`: globo → record → sony → espn → sbt → sony → record →
globo, 2 min viewing each). Per hop it asserts ALL of:
1. `POST /api/switch` → `/api/status.active_channel_id == requested`
2. TV-open: `ffmpeg -f mpegts -i /run/media/sam/LIVETV/CANAL AO VIVO.ts
   -t 8 -f null` → ≥200 frames
3. Head age: file-offset-0 video PTS vs live PTS → `-30s <= age < 240s`
   (**drop host page cache first**: stale cache faked a FAIL —
   `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches` — USB has no cache
   invalidation, the TV is unaffected, only PC-side reads lie)
4. Identity: sample freshest region `[wpos-14MB, wpos-8MB]` (heartbeat from
   `adb shell su -c 'cat /data/local/tmp/writer_pos.txt'`, file offset =
   img offset − 1593344), thumbnail must PTS-gate <120s vs live, then
   VISUALLY confirm the channel (logo/bug) in the PNG
5. Writer alive, no reconnect in `channel_stream.log` during the hop
Status when writing this: fixed sampler (wrap-proof) queued for rerun;
last run: 5/5 switches moved, 8/8 TV-open passed, identity thumbs pending
visual confirmation in `/tmp/opencode/loop_*.png`, `rt_*.png`.
Thumbnails: `ffmpeg -f mpegts -ss 2 -i sample.bin -frames:v 1 out.png`.
UPDATE 21:35 — round-trip identity loop 8/8 PASS (globo→record→sony→espn→
sbt→sony→record→globo, 2 min each): all switches moved, TV-open 207–240/240,
head age ~2min, all 8 thumbs visually confirmed (Globo/Fantástico, Record
21:16→21:29 clocks tracking real time, Sony western ×2, ESPN NFL, SBT news).
 Legge artifactual notes: skip ffmpeg `-y` overwrite prompts; drop host page
cache before head reads; heartbeat `writer_pos.txt` is truth for sampling.

## 6. Open issues / next steps
1. Re-run identity loop to all-PASS + visual thumbs (in progress infra).
2. `test_closed_loop.py` uses dead channel IDs — needs rewrite or deletion.
3. Tunnel URL fragility: any `cloudflared` restart orphan the phone writer
   (fix = Cloudflare named tunnel, needs user's CF account).
4. Phone RAM critically low (~84MB free) — watchdog covers death, but
   closing phone apps helps; reboot needs manual `~/usb_tv.sh start`.
5. Final acceptance = Samsung TV plays `CANAL AO VIVO.ts` fresh + follows
   web-remote switches (~40–60s propagation: 15MB lead + transcode latency).

## 7. Supervision Log (Claude writes here, executor reads before each block)

### 2026-09-20 — Initial brief (user)
Supervise the executor toward the main goal (§3). Priority questions we'd
like breakthroughs on:
1. Is the 15MB writer lead + 60MB window the right trade-off, or is there a
   simpler freshness scheme (e.g. writer tracks TV read position via USB
   SCSI logs instead of blind circular buffer)?
2. The head-age sawtooth (0→2.7min) means a viewer opening the file can be
   ~3 min behind live. Acceptable for the elderly viewer, or worth a
   "start-at-frontier" trick (file size/cluster hack so offset 0 maps near
   the writer)?
3. Tunnel is the fragile link (§6.3). Cheapest robust alternative given
   AP client isolation: Termux hotspot? Static LAN route? Or just accept
   Cloudflare?
4. Which of the current moving parts (prefill, keepalive nulls, restamper,
   watchdog, heartbeat) would you cut first, and what experiment would
   prove it's safe to cut?
(Append new entries below this line.)

### 2026-09-20 — Executor verification of Opus RCA (all claims tested)
1. **Issue 1 (no headers at offset 0): PARTLY TRUE, severity overstated.**
   Fresh measurement on old file: PAT@46KB, SPS/PPS/IDR@47KB — inside the
   TV's 1–2MB probe window, and ffmpeg decoded 207–240/240 from offset 0.
   "0 frames" not reproduced. Still adopted header injection (now PAT@0,
   PMT@188, SPS/PPS/IDR@376, 148/150 immediate frames) — strictly better.
2. **Issue 2 (4GB file): TRUE.** Adopted 60MB file. BUT the 64MB image was
   FAT32-illegal (15362 clusters < 65525 minimum) — rebuilt at 300MB image /
   60MB file, sector 3112 preserved. Also fixed FSInfo free count.
3. **Issue 3 (profile): cosmetic, ignored** (baseline plays on this TV).
4. **Wrap alignment: mostly forced wraps** (~75%, IDR rarely lands in the
   exact 65KB boundary chunk) — harmless: post-wrap offset 0 still decodes
   127/150, next IDR ≤1s away. Kept for the 25% win, not relied upon.
5. **deploy_fixed.sh needed fixes before use:** dead `wait-for-device`
   after UDC-none (would hang), missing UDC re-enable (would orphan ADB),
   no host-side unmount, writer restart omitted watchdog. All fixed;
   deployed manually step-by-step. Deployed state verified: 300M drive on
   host, 60MB file, v2 writer + watchdog on phone, Record News frame in
   fresh sectors post-wrap.
6. **Methodology notes for future loops:** drop host page cache before head
   reads (USB has no invalidation); sample via `writer_pos.txt` heartbeat;
   ffmpeg needs `-y` for thumbs; pidfile has no trailing newline (concats
   in shell output — caused a false 2.3TB-offset alarm).

## 8. MILESTONE 2026-09-22 — first live Samsung TV success (FUSE-direct)
Mark on the Plasma TV: audio beeps smooth, zero stutter; power-off/
reconnect + PLAY re-based to live frontier automatically and streamed on.
Backend corroborated (writer abs 1066MB monotonic, daemon serving reads,
1 listener, no events). Static-file era Findings that led here: 60MB
window cap, IDR-only wraps, PTS smoothing + rollover rebase, -re pacing,
watchdog yank-fix, adb-reverse keeper, systemd units.
FROZEN as known-good: phone fuse_direct build 23:37 (pre-synthesis),
writer fifo mode, 300MB... (see §9). Do NOT redeploy without cause.
* §9 = record phone binary md5 + repo md5s here on next plug-in.

## 9. VALIDATION COMPLETE 2026-09-22 — goal achieved
Mark: timer advancing second-by-second, smooth; ConnectShare bar pinned
00:05/00:05 (infinite-live behavior, EOF question moot); 402MB @ exact 1x,
zero stutters. Project goal met: live TV on the legacy Samsung via
emulated USB mass storage.
