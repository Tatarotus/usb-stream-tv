#!/system/bin/sh
GADGET="/config/usb_gadget/g1"
[ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
UDC=$(getprop sys.usb.controller)
MNT_POINT="/data/local/tmp/vfat_mnt"

echo "[i] Resetando processos..."
echo "" > "$GADGET/UDC" 2>/dev/null || true
echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
pkill -9 -f "[f]use_direct" 2>/dev/null || true
pkill -9 -f "[s]tream_writer.py" 2>/dev/null || true
pkill -9 -f "[w]atch_writer.sh" 2>/dev/null || true
[ -f /data/local/tmp/fuse_direct.new ] && mv /data/local/tmp/fuse_direct.new /data/local/tmp/fuse_direct && chmod 755 /data/local/tmp/fuse_direct
[ -f /data/local/tmp/fat_template.bin.new ] && mv /data/local/tmp/fat_template.bin.new /data/local/tmp/fat_template.bin
umount -l "$MNT_POINT" 2>/dev/null || true
rm -f /data/local/tmp/live_pipe /data/local/tmp/stream_abspos.txt /data/local/tmp/writer_pos.txt
mkfifo /data/local/tmp/live_pipe

echo "[i] Iniciando fuse_direct..."
mkdir -p "$MNT_POINT"
nohup /data/local/tmp/fuse_direct "$MNT_POINT" /data/local/tmp/live_pipe /data/local/tmp/fat_template.bin >/data/local/tmp/fuse_direct.log 2>&1 &
sleep 1

BACKING_IMG="$MNT_POINT/tv_stream.img"
echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file"
echo 1 > "$GADGET/functions/mass_storage.0/lun.0/removable"
echo 0 > "$GADGET/functions/mass_storage.0/lun.0/ro"
echo "LIVETV" > "$GADGET/functions/mass_storage.0/lun.0/inquiry_string"
rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true
ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1"
[ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f2" 2>/dev/null || true

echo "[i] Iniciando stream_writer..."
nohup /data/data/com.termux/files/usr/bin/python3 /data/local/tmp/stream_writer.py 3112 https://tv.smre.run.place globo-morena-dourados --fifo=/data/local/tmp/live_pipe >>/data/local/tmp/channel_stream.log 2>&1 &
nohup sh /data/local/tmp/watch_writer.sh >>/data/local/tmp/channel_stream.log 2>&1 &

echo 500000 > /sys/class/power_supply/usb/current_max 2>/dev/null || true

echo "[i] Aguardando 15s para pré-encher o buffer de reprodução inicial (evita colisão)..."
sleep 15
echo "[✓] Buffer inicial pré-carregado com sucesso!"

echo "$UDC" > "$GADGET/UDC"
echo "STARTED_OK UDC=$UDC"
