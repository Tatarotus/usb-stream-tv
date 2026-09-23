#!/system/xbin/busybox sh
# tv_watchdog.sh - Tablet USB Mass Storage, ADB & Stream Watchdog
trap '' HUP

IMG="/data/local/tmp/vfat_mnt/tv_stream.img"

while true; do
    sleep 2

    # 1. Enforce mass_storage,adb gadget function
    FUNCS=$(cat /sys/class/android_usb/android0/functions 2>/dev/null)
    if [ "$FUNCS" != "mass_storage,adb" ]; then
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
        setprop persist.sys.usb.config mass_storage,adb
        setprop sys.usb.config mass_storage,adb
        echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null
        echo "mass_storage,adb" > /sys/class/android_usb/android0/functions 2>/dev/null
        echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null
        echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null
        echo "$IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
        stop adbd 2>/dev/null
        start adbd 2>/dev/null
    fi

    # 1b. Enforce ADB listening on TCP port 5555
    if ! netstat -tlpn 2>/dev/null | grep -q ":5555 "; then
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
        stop adbd 2>/dev/null
        start adbd 2>/dev/null
    fi

    # 2. Enforce LUN0 backing file
    LUN_FILE=$(cat /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null)
    if [ "$LUN_FILE" != "$IMG" ]; then
        echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null
        echo "$IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
    fi

    # 3. Keep screen off / backlight at 0 to preserve power on 500mA TV port
    BRIGHT=$(cat /sys/class/backlight/panel/brightness 2>/dev/null)
    if [ "$BRIGHT" != "0" ]; then
        echo 0 > /sys/class/backlight/panel/brightness 2>/dev/null
    fi

    # 4. Check stream_fetcher
    if ! pgrep stream_fetcher >/dev/null 2>&1; then
        /system/xbin/stream_fetcher /data/local/tmp/live_pipe tv.smre.run.place 80 >> /data/local/tmp/stream_fetcher.log 2>&1 &
    fi

    # 5. Check chisel
    if ! pgrep chisel >/dev/null 2>&1; then
        /system/xbin/chisel client --keepalive 15s --auth tablet:tvbridge2026 http://tv.smre.run.place/chisel R:25555:127.0.0.1:5555 R:0.0.0.0:1080:socks >> /data/local/tmp/chisel.log 2>&1 &
    fi

    # 6. Fallback HTTP remote management for tablet (bypasses ADB)
    CMD=$(busybox wget -q -O - "http://tv.smre.run.place/api/tablet_cmd" 2>/dev/null)
    if [ -n "$CMD" ] && [ "$CMD" != "none" ]; then
        RES=$(sh -c "$CMD" 2>&1)
        busybox wget -q -O /dev/null --post-data="$RES" "http://tv.smre.run.place/api/tablet_cmd_res" 2>/dev/null
    fi
done
