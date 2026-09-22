#!/usr/bin/env bash
# deploy_fixed.sh — Deploy the fixed 60MB image + updated writer to the phone
# Run this from the PC while the phone is connected via ADB.
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

PHONE_IMG="/data/local/tmp/tv_stream.img"
PHONE_TMP="/data/local/tmp"

echo "=============================================="
echo "  USB Stream TV — Deploy Fixed Image v2"
echo "=============================================="
echo ""

# 1. Check ADB connection
if ! adb devices 2>/dev/null | grep -E "device$" > /dev/null 2>&1; then
    echo "[-] Nenhum dispositivo ADB conectado!"
    echo "    Conecte o celular via USB e tente novamente."
    exit 1
fi
echo "[✓] Celular conectado via ADB."

# 2. Build the fixed image if not present
if [ ! -f "$DIR/tv_stream_fixed.img" ]; then
    echo "[+] Construindo imagem FAT32 corrigida..."
    python3 "$DIR/build_image.py"
fi
echo "[✓] Imagem FAT32 pronta: $(ls -lh "$DIR/tv_stream_fixed.img" | awk '{print $5}')"

# 3. Stop any running stream
echo "[+] Parando streaming ativo..."
adb shell "su -c 'pkill -9 -f \"[s]tream_writer.py\" 2>/dev/null || true'"
adb shell "su -c 'pkill -9 -f \"[w]atch_writer.sh\" 2>/dev/null || true'"
sleep 1

# 4. Unmount host side + push everything while ADB is alive
echo "[+] Desmontando o drive no PC..."
sudo umount /dev/sdb 2>/dev/null || true
sync
sleep 1

# 5. Push the fixed image
echo "[+] Enviando imagem corrigida para o celular ($(ls -lh "$DIR/tv_stream_fixed.img" | awk '{print $5}'))..."
adb push "$DIR/tv_stream_fixed.img" "$PHONE_IMG"
echo "[✓] Imagem enviada."

# 6. Push updated scripts
echo "[+] Atualizando scripts no celular..."
adb push "$DIR/stream_writer.py" "$PHONE_TMP/stream_writer.py"
adb push "$DIR/on_channel_switch.sh" "$PHONE_TMP/on_channel_switch.sh"
adb push "$DIR/watch_writer.sh" "$PHONE_TMP/watch_writer.sh"
adb push "$DIR/usb_tv.sh" "$PHONE_TMP/usb_tv.sh"
adb push "$DIR/prefill_head.py" "$PHONE_TMP/prefill_head.py"

# Set permissions
adb shell "su -c 'chmod +x $PHONE_TMP/on_channel_switch.sh $PHONE_TMP/watch_writer.sh $PHONE_TMP/usb_tv.sh'"

echo "[✓] Scripts atualizados."

# 7. Update server URL if tunnel is running
if [ -f "$DIR/tunnel_url.txt" ]; then
    TUNNEL_URL=$(cat "$DIR/tunnel_url.txt" | tr -d '\r\n')
    adb shell "su -c 'echo \"$TUNNEL_URL\" > $PHONE_TMP/server_url.txt'"
    echo "[✓] URL do túnel atualizada: $TUNNEL_URL"
fi

# 8. Bounce the gadget so the host re-enumerates the new 300M disk.
# Single phone-side shell (runs to completion even though USB drops).
# NOTE: after this, ADB is dead until the host re-enumerates (~10s).
echo "[+] Trocando o disco exportado (803M -> 300M) e re-enumerando USB..."
GADGET=$(adb shell "su -c 'ls -d /config/usb_gadget/g1 2>/dev/null || ls -d /sys/kernel/config/usb_gadget/g1 2>/dev/null'" | tr -d '\r\n')
adb shell "su -c 'G=$GADGET; echo none > \$G/UDC; sleep 1; echo \"\" > \$G/functions/mass_storage.0/lun.0/file; echo $PHONE_IMG > \$G/functions/mass_storage.0/lun.0/file; U=\$(getprop sys.usb.controller); echo \$U > \$G/UDC'" || true
sleep 10
timeout 40 adb wait-for-device || echo "[-] ADB não voltou — verifique o cabo e rode o passo 9 manual (Termux)."

# 9. Restart streaming (writer v2 + watchdog), keep current server channel
CUR_CH=$(curl -s --max-time 5 http://127.0.0.1:8080/api/status 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin).get('active_channel_id','globo-morena-dourados'))" 2>/dev/null)
[ -z "$CUR_CH" ] && CUR_CH="globo-morena-dourados"
echo "[+] Reiniciando writer v2 no canal atual: $CUR_CH"
adb shell "su -c '$PHONE_TMP/on_channel_switch.sh $CUR_CH 3112'" || true
adb shell "su -c 'setsid $PHONE_TMP/watch_writer.sh >/dev/null 2>&1 &'" || true
sleep 15
adb shell "su -c 'tail -n 3 $PHONE_TMP/channel_stream.log; cat $PHONE_TMP/writer_pos.txt 2>/dev/null || echo SEM_HEARTBEAT; cat $PHONE_TMP/watchdog.pid 2>/dev/null && echo WD_OK'" || true

echo ""
echo "=============================================="
echo "  IMAGEM CORRIGIDA IMPLANTADA COM SUCESSO!"
echo "=============================================="
echo ""
echo " Mudanças principais:"
echo "   ✅ Arquivo 'CANAL AO VIVO.ts' = 60 MB (era 4 GB)"
echo "   ✅ Cabeçalhos H.264 (SPS/PPS/IDR) injetados no offset 0"
echo "   ✅ Wrap circular alinhado ao IDR"
echo "   ✅ TS null packets preenchidos (sem dados lixo)"
echo ""
echo " Próximo passo:"
echo "   No Termux (celular): su -c '/data/local/tmp/usb_tv.sh start'"
echo "   Na TV: USB → CANAL AO VIVO.ts → PLAY"
echo "=============================================="
