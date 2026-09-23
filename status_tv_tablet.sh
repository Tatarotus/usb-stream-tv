#!/system/bin/sh
# status_tv.sh - Check USB Stream TV Status

echo "=== PROCESSES ==="
ps | grep -E 'fuse_direct|stream_fetcher|chisel'
echo ""
echo "=== GADGET STATUS ==="
echo "enable: $(cat /sys/class/android_usb/android0/enable 2>/dev/null)"
echo "functions: $(cat /sys/class/android_usb/android0/functions 2>/dev/null)"
echo "lun0 file: $(cat /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null)"
echo "lun0 ro: $(cat /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null)"
echo ""
echo "=== LOGS ==="
echo "--- fuse.log ---"
tail -n 3 /data/local/tmp/fuse.log 2>/dev/null
echo "--- stream_fetcher.log ---"
tail -n 3 /data/local/tmp/stream_fetcher.log 2>/dev/null
echo "--- chisel.log ---"
tail -n 3 /data/local/tmp/chisel.log 2>/dev/null
