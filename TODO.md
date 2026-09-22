 ### What the Next Agent Needs to Do

  1. Read the handover doc + read the skill at /home/sam/.agents/skills/usb-stream-tv/SKILL.
  md
  2. Implement fuse_sensor_v2.c — the ring buffer approach described in Section 3 (the key
  architectural change that fixes the lag)
  3. Recreate the channel list — may want different channels, needs new FAT32 image + sector
  mapping
  4. Set up the new device — follow the checklist in Section 9 (verify ConfigFS, FUSE, CPU
  arch)
  5. Optionally deploy to VPS — Section 9 has the systemd + nginx config
  6. Implement Movies/VOD — Section 10 has the design (add cinema tab to web remote)
