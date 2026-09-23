#!/bin/bash
# fix_tablet_adb.sh - Restore & permanently harden tablet ADB over tunnel
set -e

echo "[*] Waiting for tablet on USB (booted or TWRP)..."
while true; do
    DEV=$(adb devices | grep -E "device$|recovery$" | head -n 1 | awk '{print $1}')
    if [ -n "$DEV" ] && [ "$DEV" != "127.0.0.1:25555" ]; then
        echo "[✓] Found device on USB: $DEV"
        break
    fi
    sleep 1
done

STATE=$(adb devices | grep "$DEV" | awk '{print $2}')
echo "[*] Device state: $STATE"

if [ "$STATE" = "recovery" ]; then
    echo "[*] Device is in TWRP Recovery mode."
    adb -s "$DEV" shell "mount /system 2>/dev/null || mount /dev/block/platform/*/by-name/SYSTEM /system 2>/dev/null || true"
    adb -s "$DEV" shell "mount /data 2>/dev/null || true"
    
    echo "[*] Pushing updated start_tv.sh and tv_watchdog.sh..."
    adb -s "$DEV" push start_tv_tablet.sh /system/xbin/start_tv.sh
    adb -s "$DEV" push tv_watchdog.sh /system/xbin/tv_watchdog.sh
    adb -s "$DEV" shell "chmod 755 /system/xbin/start_tv.sh /system/xbin/tv_watchdog.sh"
    
    echo "[*] Setting persistent properties..."
    adb -s "$DEV" shell "echo '5555' > /data/property/persist.adb.tcp.port 2>/dev/null || true"
    adb -s "$DEV" shell "echo 'mass_storage,adb' > /data/property/persist.sys.usb.config 2>/dev/null || true"
    adb -s "$DEV" shell "chmod 600 /data/property/persist.* 2>/dev/null || true"
    
    echo "[✓] Config updated! Rebooting tablet to system..."
    adb -s "$DEV" reboot
else
    echo "[*] Device is in normal Android mode."
    echo "[*] Remounting /system rw..."
    adb -s "$DEV" shell "su -c 'mount -o remount,rw /system'"
    
    echo "[*] Pushing updated scripts..."
    adb -s "$DEV" push start_tv_tablet.sh /data/local/tmp/start_tv.sh
    adb -s "$DEV" push tv_watchdog.sh /data/local/tmp/tv_watchdog.sh
    adb -s "$DEV" shell "su -c '
        cp /data/local/tmp/start_tv.sh /system/xbin/start_tv.sh
        cp /data/local/tmp/tv_watchdog.sh /system/xbin/tv_watchdog.sh
        chmod 755 /system/xbin/start_tv.sh /system/xbin/tv_watchdog.sh
        mount -o remount,ro /system
        
        # Enforce persistent TCP 5555 and mass_storage,adb
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
        setprop persist.sys.usb.config mass_storage,adb
        setprop sys.usb.config mass_storage,adb
        stop adbd
        start adbd
        
        killall -9 tv_watchdog.sh 2>/dev/null
        /system/xbin/tv_watchdog.sh >/dev/null 2>&1 &
    '"
fi

echo "[*] Waiting 5 seconds for adbd to initialize on port 5555..."
sleep 5

echo "[*] Checking local wireless ADB connection via Oracle VPS tunnel..."
adb disconnect 127.0.0.1:25555 2>/dev/null || true
adb connect 127.0.0.1:25555
adb -s 127.0.0.1:25555 shell "su -c 'id; echo [✓] Tablet online and hardened!'"
