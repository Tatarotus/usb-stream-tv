# fuse_direct — SPEC v1 (FUSE-served infinite live file)

## Goal
TV reads NEVER hit EOF, NEVER overtake the writer, NEVER see torn rewrites.
File appears as one ~4GB `TV AO VIVO.ts` on a virtual FAT32 disk; every byte
served from a 32MB RAM ring fed by `writer.py` over a named FIFO.

## Why not a bigger static file
Any finite file reintroduces EOF (reader stops) or stale regions. Only an
endless byte stream removes the whole defect class. Repeat-one stays as a
backstop, not the mechanism.

## Components (phone, root, aarch64 static C + existing python)
1. `writer.py` (modified): POST switch, reconnect, header scan stay. Sectio
   sector writes REPLACED by sequential blocking writes to
   `/data/local/tmp/live_pipe` (named FIFO, raw TS bytes, nothing else).
   Absolute stream position `S` (bytes since writer start); heartbeat file
   gains `S`. No wrap logic anymore (the ring wraps, positions don't).
2. `fuse_direct` (new C, raw kernel FUSE protocol like `fuse_sensor.c`,
   `-static -O2 -lpthread`): mounts one file (the whole virtual disk image,
   ~4.3GB virtual, ~100KB real RAM + 32MB ring). USB gadget LUN points at
   the FUSE file (same as v1 architecture).
3. `watch_writer.sh` (unchanged): monitors writer process. Watchdog does NOT
   need to monitor the daemon (daemon is stateless; dies → TV stalls →
   watchdog restarts writer → fifo EOF → daemon resets ring, see §6).
4. `fat_template.bin` (generated once by python): boot + FSInfo + root-dir
   sectors copied from current fixed image, patched at load: total sectors
   for ~4.3GB disk, FAT size 8192 sectors, file size 0xFFFFFFFF
   (4,294,967,295 = 3.1h at 2.9Mbps — practical infinity + repeat backstop).

## Virtual disk layout (numbers)
- bytes/sector 512, sectors/cluster 8 (4096), reserved 32, FATs 2,
  sectors/FAT 8192, root dir cluster 2, file data starts cluster 3.
- Data region starts sector 32+2*8192 = 16416. File byte F lives at
  sector 16416 + (F/4096)*8 + (F%4096)/512 (F contiguous — chain implicit).
- FAT sectors served SYNTHESIZED (never stored): entry for cluster c =
  c+1 for 3 <= c < 3+N, EOF (0x0FFFFFFF) at 3+N, free (0) elsewhere,
  N = file_bytes/4096. Media/reserved entries (0,1) + root (2, EOF) fixed.
- Sectors outside [boot, FATs, rootdir, file-data] → zeros.

## Ring + read mapping (the core)
- Ring 32MB (~90s). Writer appends at absolute S_write (grows forever).
- Per-open rebase: on FUSE_OPEN, set `base = max(0, S_write - 2MB)`.
  File offset F → stream byte `base + F`.
- FUSE_READ(F, size):
  - start = base+F. If start+size <= S_write and start >= S_write-32MB:
    copy (two parts if wrapping ring). Never torn: copy under mutex.
  - If start >= S_write (ahead of frontier): BLOCK on condvar until
    S_write catches up (writer pace = exact 1x broadcast rate) or 10s
    timeout → return zeros for the missing tail (never hang the SCSI bus).
  - If start < S_write-32MB (overwritten history): return zeros.
- TV opens at 0 → starts ≤ ~7s behind live (2MB), rides 1x forever, never
  EOF (4GB ≈ 3.1h; repeat backstop beyond that).

## Channel switch / writer restart
- Same file, same positions continue (server PTS continuous) → ~1s splice,
  identical to today. NVRAM filename unchanged.
- Writer death → fifo EOF → daemon marks epoch invalid, resets ring,
  zeroes reads until writer returns; next FUSE_OPEN re-bases. TV glitches
  ~2s max, once per writer restart (rare: watchdog-covered).

## FUSE opcodes to implement
INIT, LOOKUP (1 name), GETATTR (root + file, size = 0xFFFFFFFF file /
disk size for root), OPEN (rebase + fh=1), READ (above), RELEASE, FORGET
(noop), plus ENOSYS default. Direct port of existing `fuse_sensor.c`
boilerplate for INIT/LOOKUP/GETATTR/OPEN.

## Build / deploy
- `aarch64-linux-gnu-gcc -static -O2 -lpthread fuse_direct.c -o fuse_direct`
- Phone: `mkfifo /data/local/tmp/live_pipe`, push daemon + writer +
  template, start daemon (setsid, oom -1000), start writer, point LUN at
  FUSE file, UDC bounce.
- Rollback: LUN back to `tv_stream.img`, kill daemon (old path untouched).

## Test plan (mpv acceptance BEFORE tv)
1. Infinite read: `dd` 200MB sequentially from FUSE file → 100% 0x47 sync
   (phase-adjusted), PTS monotonic except ≤1 wrap-equivalent per 90s.
2. Block pacing: read 5MB past frontier → returns after ~14s (data-paced),
   not instantly (zeros) or hang.
3. Open rebase: two opens 60s apart both start ≤10s behind live.
4. Switch + writer-kill recovery: ≤3s glitch, then clean.
5. mpv 5-min loop test on FUSE file (same gate as static file).
Only then: Samsung TV + repeat-one.

## Open questions for Gemini
1. 32MB ring (~90s) vs 64MB (~3min): bigger rides out longer tunnel
   stalls, costs RAM (phone has ~84MB free — 32MB is the safe ceiling?).
2. EOF at 4GB/3.1h: accept + repeat backstop, or NTFS-synth for more?
3. Per-open rebase races a TV that holds one open for hours: base goes
   stale (file start falls >32MB behind frontier → zeros if TV seeks to
   0 hours later). Mitigate: re-base lazily when reads arrive >32MB
   behind? Propose rule.
