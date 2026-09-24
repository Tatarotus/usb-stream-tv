#!/system/bin/sh
# switch_vod.sh — Alternador para Modo VOD (Cloud Virtual Remote Disk)
# Suporta tanto o Tablet SM-T110 quanto o Xiaomi Mi A2
[ ! -x "/system/bin/sh" ] && exec /system/xbin/busybox sh "$0" "$@"

TASK_ID="$1"
if [ -z "$TASK_ID" ]; then
    echo "Uso: switch_vod.sh <task_id> [host_or_ip]"
    exit 1
fi

SERVER_HOST="${2:-tv.smre.run.place}"
LOCAL_DIR="/data/local/tmp"
MNT_POINT="$LOCAL_DIR/vfat_mnt"
FLAG_FILE="$LOCAL_DIR/vod_mode.flag"
BACKING_IMG="$MNT_POINT/tv_stream.img"

echo "[*] Ativando Modo VOD (Tarefa: $TASK_ID)..."
echo "$TASK_ID" > "$FLAG_FILE"

# 1. Parar streaming ao vivo para liberar rede e CPU
pkill -9 -f "[s]tream_fetcher" 2>/dev/null || true
pkill -9 -f "[s]tream_writer.py" 2>/dev/null || true
sleep 1

# 2. Baixar o template FAT32 leve da tarefa (~5.5 KB)
TMPL_URL="http://$SERVER_HOST/vod/$TASK_ID/template.bin"
TMPL_LOCAL="$LOCAL_DIR/fat_template_vod.bin"
echo "[*] Baixando template FAT32 de $TMPL_URL..."

if [ -x "/system/xbin/busybox" ]; then
    /system/xbin/busybox wget -q -O "$TMPL_LOCAL" "$TMPL_URL"
elif [ -x "$LOCAL_DIR/curl" ]; then
    "$LOCAL_DIR/curl" -s -o "$TMPL_LOCAL" "$TMPL_URL"
elif [ -x "/data/data/com.termux/files/usr/bin/curl" ]; then
    /data/data/com.termux/files/usr/bin/curl -s -o "$TMPL_LOCAL" "$TMPL_URL"
else
    wget -q -O "$TMPL_LOCAL" "$TMPL_URL" 2>/dev/null || curl -s -o "$TMPL_LOCAL" "$TMPL_URL"
fi

if [ ! -s "$TMPL_LOCAL" ]; then
    echo "[!] Falha ao baixar template FAT32. Abortando."
    rm -f "$FLAG_FILE"
    exit 1
fi
echo "[✓] Template FAT32 carregado com sucesso."

# 3. Parar FUSE atual e desmontar
pkill -9 -f "[f]use_direct" 2>/dev/null || true
umount -l "$MNT_POINT" 2>/dev/null || true
sleep 1

# 4. Selecionar binário FUSE de acordo com a arquitetura
ARCH=$(uname -m 2>/dev/null || echo "armv7l")
case "$ARCH" in
    armv7*|aarch32*)
        FUSE_BIN="$LOCAL_DIR/fuse_direct_arm32"
        [ ! -x "$FUSE_BIN" ] && FUSE_BIN="/system/xbin/fuse_direct"
        ;;
    aarch64*|arm64*)
        FUSE_BIN="$LOCAL_DIR/fuse_direct_arm64"
        [ ! -x "$FUSE_BIN" ] && FUSE_BIN="$LOCAL_DIR/fuse_direct"
        ;;
    *)
        FUSE_BIN="$LOCAL_DIR/fuse_direct"
        ;;
esac

if [ ! -x "$FUSE_BIN" ]; then
    echo "[!] Binário FUSE compatível não encontrado ($FUSE_BIN)."
    rm -f "$FLAG_FILE"
    exit 1
fi

# 5. Iniciar FUSE em Modo VOD Cloud (HTTP Range)
VOD_STREAM_URL="http://$SERVER_HOST/vod/$TASK_ID/movie.mp4"
echo "[*] Iniciando $FUSE_BIN em $MNT_POINT..."
mkdir -p "$MNT_POINT"
nohup "$FUSE_BIN" "$MNT_POINT" "$VOD_STREAM_URL" "$TMPL_LOCAL" > "$LOCAL_DIR/fuse_vod.log" 2>&1 &
sleep 2

# 6. Soft-reset no USB Gadget para a TV detectar o novo arquivo
echo "[*] Reiniciando barramento USB para varredura da TV..."
if [ -d "/config/usb_gadget/g1" ] || [ -d "/sys/kernel/config/usb_gadget/g1" ]; then
    # Xiaomi Mi A2 (ConfigFS moderno)
    GADGET="/config/usb_gadget/g1"
    [ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
    UDC=$(getprop sys.usb.controller)
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    sleep 1
    echo 1 > "$GADGET/functions/mass_storage.0/lun.0/ro" 2>/dev/null || true
    echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
    rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true
    ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1" 2>/dev/null || true
    [ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f2" 2>/dev/null || true
    echo 500000 > /sys/class/power_supply/usb/current_max 2>/dev/null || true
    echo "$UDC" > "$GADGET/UDC" 2>/dev/null || true
elif [ -d "/sys/class/android_usb/android0/f_mass_storage" ]; then
    # Tablet SM-T110 (sysfs legado)
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true
    sleep 1
    echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
    echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true
fi

echo "[✓] VOD Ativado na TV com Sucesso! TV já está executando varredura."
