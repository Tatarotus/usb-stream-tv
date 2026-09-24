#!/system/bin/sh
# switch_live.sh — Retorna do Modo VOD para a TV Ao Vivo
# Suporta tanto o Tablet SM-T110 quanto o Xiaomi Mi A2
[ ! -x "/system/bin/sh" ] && exec /system/xbin/busybox sh "$0" "$@"

LOCAL_DIR="/data/local/tmp"
FLAG_FILE="$LOCAL_DIR/vod_mode.flag"

echo "[*] Restaurando modo TV Ao Vivo..."
rm -f "$FLAG_FILE"

# Parar FUSE VOD
pkill -9 -f "[f]use_direct" 2>/dev/null || true
umount -l "$LOCAL_DIR/vfat_mnt" 2>/dev/null || true
sleep 1

# Executa o inicializador limpo correspondente ao aparelho
if [ -x "/system/xbin/start_tv.sh" ]; then
    echo "[*] Reiniciando stack do Tablet..."
    /system/xbin/start_tv.sh
elif [ -x "$LOCAL_DIR/start_clean.sh" ]; then
    echo "[*] Reiniciando stack do Xiaomi Mi A2..."
    sh "$LOCAL_DIR/start_clean.sh"
fi

echo "[✓] TV Ao Vivo restaurada na TV com sucesso!"
