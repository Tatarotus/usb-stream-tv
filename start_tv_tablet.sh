#!/system/xbin/busybox sh
# start_tv.sh - USB Stream TV Bridge & Management Service on Tablet
trap '' HUP

killall -9 stream_fetcher fuse_direct chisel tv_watchdog.sh 2>/dev/null
sleep 1

# Enforce default USB Mass Storage
setprop persist.sys.usb.config mass_storage,adb
setprop sys.usb.config mass_storage,adb
echo 0 > /sys/class/backlight/panel/brightness 2>/dev/null

# 1. Setup FUSE mountpoint & FIFO

mkdir -p /data/local/tmp/vfat_mnt
cp -f /system/etc/fat_template.bin /data/local/tmp/fat_template.bin
chmod 644 /data/local/tmp/fat_template.bin
rm -f /data/local/tmp/live_pipe
/system/xbin/busybox mkfifo /data/local/tmp/live_pipe
chmod 666 /data/local/tmp/live_pipe
chmod 666 /dev/fuse

# 2. Start fuse_direct
/system/xbin/fuse_direct /data/local/tmp/vfat_mnt /data/local/tmp/live_pipe /system/etc/fat_template.bin > /data/local/tmp/fuse.log 2>&1 &
sleep 1

# 3. Configure USB Gadget
FUNCS=$(cat /sys/class/android_usb/android0/functions 2>/dev/null)
case "$FUNCS" in
    *mass_storage*)
        ;;
    *)
        echo 0 > /sys/class/android_usb/android0/enable
        echo "mass_storage,adb" > /sys/class/android_usb/android0/functions
        echo 1 > /sys/class/android_usb/android0/enable
        sleep 1
        ;;
esac

echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null
echo "/data/local/tmp/vfat_mnt/tv_stream.img" > /sys/class/android_usb/android0/f_mass_storage/lun0/file

# 4. Ensure adbd TCP is enabled on port 5555
setprop service.adb.tcp.port 5555

# 5. Start Chisel Reverse Tunnel to Oracle VPS
/system/xbin/chisel client --keepalive 15s --auth tablet:tvbridge2026 http://tv.smre.run.place/chisel R:25555:127.0.0.1:5555 R:0.0.0.0:1080:socks > /data/local/tmp/chisel.log 2>&1 &


# 6. Start stream_fetcher
/system/xbin/stream_fetcher /data/local/tmp/live_pipe tv.smre.run.place 80 > /data/local/tmp/stream_fetcher.log 2>&1 &

# 7. Start USB Mass Storage & Process Watchdog
/system/xbin/tv_watchdog.sh > /data/local/tmp/tv_watchdog.log 2>&1 &

echo "[✓] USB Stream TV stack started!"

