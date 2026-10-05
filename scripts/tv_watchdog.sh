#!/system/xbin/busybox sh
# tv_watchdog.sh - Tablet USB Mass Storage, ADB & Stream Watchdog
trap '' HUP

IMG="/data/local/tmp/vfat_mnt/tv_stream.img"
CMD_TICK=0

while true; do
    sleep 2

    # Detect current backing image (NTFS or FAT32)
    if /system/xbin/busybox pgrep -f fuse_direct >/dev/null 2>&1; then
        IMG="/data/local/tmp/vfat_mnt/tv_stream.img"
        INQ="CINEMA"
    elif [ -f "/data/local/tmp/ntfs_lab/mnt/tv_stream.img" ]; then
        IMG="/data/local/tmp/ntfs_lab/mnt/tv_stream.img"
        CUR_INQ=$(cat /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null)
        case "$CUR_INQ" in
            LIVETV1|LIVETV2) INQ="$CUR_INQ" ;;
            *) INQ="LIVETV1" ;;
        esac
        [ -f "/data/local/tmp/vod_mode.flag" ] && INQ="CINEMA"
        [ -f "/data/local/tmp/favorites_mode.flag" ] && INQ="CHANNELS"
    else
        IMG="/data/local/tmp/vfat_mnt/tv_stream.img"
        INQ="LIVETV1"
        [ -f "/data/local/tmp/favorites_mode.flag" ] && INQ="CHANNELS"
    fi

    # 0. Enforce wake_lock, performance governor, and Wi-Fi power off to avoid deep sleep jitter
    if ! grep -q "tv_stream" /sys/power/wake_lock 2>/dev/null; then
        echo "tv_stream" > /sys/power/wake_lock 2>/dev/null || true
    fi
    GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    if [ "$GOV" != "performance" ]; then
        for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo performance > "$g" 2>/dev/null || true
        done
        /system/xbin/iwconfig wlan0 power off 2>/dev/null || true
    fi

    # 1. Enforce mass_storage,adb gadget function
    FUNCS=$(cat /sys/class/android_usb/android0/functions 2>/dev/null)
    if [ "$FUNCS" != "mass_storage,adb" ]; then
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
        setprop persist.sys.usb.config mass_storage,adb
        setprop sys.usb.config mass_storage,adb
        echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null
        echo "mass_storage,adb" > /sys/class/android_usb/android0/functions 2>/dev/null
        echo "$INQ" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
        echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
        echo 0 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null
        echo "$IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
        echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null
        stop adbd 2>/dev/null
        start adbd 2>/dev/null
    fi

    # 1b. Enforce ADB listening on TCP port 5555 (0x15B3 in hex, zero overhead via /proc/net/tcp)
    if ! grep -q " 00000000:15B3 " /proc/net/tcp 2>/dev/null; then
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
        stop adbd 2>/dev/null
        start adbd 2>/dev/null
    fi



    # 3. Keep screen off / backlight at 0 to preserve power on 500mA TV port
    BRIGHT=$(cat /sys/class/backlight/panel/brightness 2>/dev/null)
    if [ "$BRIGHT" != "0" ]; then
        echo 0 > /sys/class/backlight/panel/brightness 2>/dev/null
    fi

    # Check if mode switch is currently in progress
    if [ -f "/data/local/tmp/mode_switch.lock" ]; then
        sleep 2
        continue
    fi

    # Fast HTTP remote management for tablet (checks every 2 seconds) with TV USB/power telemetry
    USB_ST=$(cat /sys/class/android_usb/android0/state 2>/dev/null)
    USB_PWR=$(cat /sys/class/power_supply/usb/online 2>/dev/null)
    CMD=$(busybox wget -q -O - "http://tv.smre.run.place/api/tablet_cmd?usb=${USB_ST:-DISCONNECTED}&pwr=${USB_PWR:-0}" 2>/dev/null)
    if [ -n "$CMD" ] && [ "$CMD" != "none" ]; then
        RES=$(sh -c "$CMD" 2>&1)
        busybox wget -q -O /dev/null --post-data="$RES" "http://tv.smre.run.place/api/tablet_cmd_res" 2>/dev/null
    fi



    # 5. Check chisel
    if ! pgrep chisel >/dev/null 2>&1; then
        /system/xbin/chisel client --keepalive 15s --auth tablet:tvbridge2026 http://tv.smre.run.place/chisel R:25555:127.0.0.1:5555 R:0.0.0.0:1080:socks >> /data/local/tmp/chisel.log 2>&1 &
    fi
done
