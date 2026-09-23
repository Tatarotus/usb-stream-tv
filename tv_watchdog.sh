#!/system/xbin/busybox sh
# tv_watchdog.sh - Tablet USB Mass Storage & Stream Watchdog
trap '' HUP

IMG="/data/local/tmp/vfat_mnt/tv_stream.img"

while true; do
    sleep 2

    # 1. Enforce mass_storage gadget function
    FUNCS=$(cat /sys/class/android_usb/android0/functions 2>/dev/null)
    case "$FUNCS" in
        *mass_storage*)
            ;;
        *)
            setprop persist.sys.usb.config mass_storage,adb
            setprop sys.usb.config mass_storage,adb
            echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null
            echo "$IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
            ;;
    esac

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
done
