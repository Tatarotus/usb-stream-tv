#!/system/bin/sh
# /system/etc/install-recovery-2.sh - Auto-start USB Stream TV on device boot
(
    sleep 15
    /system/xbin/start_tv.sh
) &
