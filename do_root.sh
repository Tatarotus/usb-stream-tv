#!/system/bin/sh
exec > /data/local/tmp/root_log.txt 2>&1
export PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin:$PATH

echo "--- START ---"
/system/bin/id
/system/bin/mount -o remount,rw /system
echo "mount exit: $?"

/system/bin/cat /data/local/tmp/su > /system/xbin/su
echo "cat xbin exit: $?"
/system/bin/chown 0.0 /system/xbin/su
/system/bin/chmod 6755 /system/xbin/su

/system/bin/cat /data/local/tmp/su > /system/bin/su
echo "cat bin exit: $?"
/system/bin/chown 0.0 /system/bin/su
/system/bin/chmod 6755 /system/bin/su

echo "--- FINISH ---"
