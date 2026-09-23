#!/system/bin/sh
# stop_tv.sh - Stop USB Stream TV Bridge Service

killall -9 stream_fetcher fuse_direct 2>/dev/null
echo 0 > /sys/class/android_usb/android0/enable
echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null
echo "mtp,acm,adb" > /sys/class/android_usb/android0/functions
echo 1 > /sys/class/android_usb/android0/enable
echo "[✓] USB Stream TV stopped."
