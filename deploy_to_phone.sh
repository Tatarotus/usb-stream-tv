#!/usr/bin/env bash
# deploy_to_phone.sh — Implantação completa do USB Stream TV (FUSE Direct) no celular via ADB
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHONE_TMP="/data/local/tmp"

echo "=================================================="
echo "  USB Stream TV — Deploy FUSE Direct para Celular"
echo "=================================================="

if ! adb devices 2>/dev/null | grep -E "device$" > /dev/null 2>&1; then
    echo "[-] Nenhum dispositivo ADB detectado!"
    echo "    Conecte o celular via USB ou ADB Wi-Fi e tente novamente."
    exit 1
fi

echo "[✓] Celular detectado via ADB."

# 1. Compila fuse_direct se não existir
if [ ! -f "$DIR/fuse_direct" ]; then
    echo "[+] Compilando fuse_direct para aarch64..."
    aarch64-linux-gnu-gcc -static -O2 -lpthread "$DIR/fuse_direct.c" -o "$DIR/fuse_direct"
fi

# 2. Gera fat_template.bin se não existir
if [ ! -f "$DIR/fat_template.bin" ]; then
    echo "[+] Gerando template FAT32..."
    python3 "$DIR/gen_template.py"
fi

# 3. Envia arquivos essenciais para o celular (< 1MB no total)
echo "[+] Enviando motor FUSE Direct e scripts..."
adb push "$DIR/fuse_direct" "$PHONE_TMP/fuse_direct"
adb push "$DIR/fat_template.bin" "$PHONE_TMP/fat_template.bin"
adb push "$DIR/stream_writer.py" "$PHONE_TMP/stream_writer.py"
adb push "$DIR/watch_writer.sh" "$PHONE_TMP/watch_writer.sh"
adb push "$DIR/on_channel_switch.sh" "$PHONE_TMP/on_channel_switch.sh"
adb push "$DIR/usb_tv.sh" "$PHONE_TMP/usb_tv.sh"

# 4. Ajusta permissões e cria atalho no Termux
adb shell "su -c 'chmod +x $PHONE_TMP/fuse_direct $PHONE_TMP/on_channel_switch.sh $PHONE_TMP/watch_writer.sh $PHONE_TMP/usb_tv.sh'"
adb shell "su -c 'cp $PHONE_TMP/usb_tv.sh /data/data/com.termux/files/home/tv 2>/dev/null && chmod +x /data/data/com.termux/files/home/tv 2>/dev/null || true'"

echo "[✓] Deploy concluído com sucesso!"
echo "    Para iniciar no celular, basta abrir o Termux e rodar:"
echo "    su"
echo "    ./tv start"
echo "=================================================="
