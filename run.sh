#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

CLOUDFLARED="$DIR/bin/cloudflared"
PORT=8080
PID_SERVER=""
PID_TUNNEL=""

cleanup() {
    echo ""
    echo "[!] Encerrando serviços..."
    [ -n "$PID_SERVER" ] && kill "$PID_SERVER" 2>/dev/null || true
    [ -n "$PID_TUNNEL" ] && kill "$PID_TUNNEL" 2>/dev/null || true
    rm -f tunnel.log
    exit 0
}

trap cleanup INT TERM EXIT

echo "=================================================="
echo "  Iniciando USB Stream TV (Servidor + Túnel)"
echo "=================================================="

# 1. Inicia o servidor Python
echo "[+] Iniciando servidor de streaming na porta $PORT..."
python3 "$DIR/server.py" &
PID_SERVER=$!
sleep 1

# 2. Inicia o Cloudflare Tunnel
echo "[+] Iniciando túnel seguro com Cloudflare..."
rm -f tunnel.log tunnel_url.txt
"$CLOUDFLARED" tunnel --url "http://127.0.0.1:$PORT" > tunnel.log 2>&1 &
PID_TUNNEL=$!

# 3. Aguarda o link público
echo "[+] Conectando à rede global da Cloudflare..."
TUNNEL_URL=""
for i in {1..30}; do
    if grep -E "https://[a-zA-Z0-9-]+\.trycloudflare\.com" tunnel.log > /dev/null 2>&1; then
        TUNNEL_URL=$(grep -oE "https://[a-zA-Z0-9-]+\.trycloudflare\.com" tunnel.log | head -n 1)
        break
    fi
    sleep 1
done

if [ -z "$TUNNEL_URL" ]; then
    echo "[-] Erro ao obter URL do túnel. Verifique tunnel.log."
    exit 1
fi

echo "$TUNNEL_URL" > "$DIR/tunnel_url.txt"

# 4. Atualiza scripts e URL no celular se estiver conectado via ADB
STREAM_URL="$TUNNEL_URL/live.ts"
if command -v adb > /dev/null 2>&1 && adb devices | grep -E "device$" > /dev/null 2>&1; then
    echo "[+] Celular detectado via ADB! Atualizando configurações..."

    # Grava URL do tunnel para o on_channel_switch.sh usar como fallback
    adb shell "su -c 'echo \"$TUNNEL_URL\" > /data/local/tmp/server_url.txt'" || true

    # Atualiza scripts com versão mais recente
    adb push "$DIR/on_channel_switch.sh" /data/local/tmp/on_channel_switch.sh > /dev/null 2>&1 || true
    adb push "$DIR/fuse_sensor" /data/local/tmp/fuse_sensor > /dev/null 2>&1 || true
    adb push "$DIR/usb_tv.sh" /data/local/tmp/usb_tv.sh > /dev/null 2>&1 || true
    adb shell "su -c 'chmod +x /data/local/tmp/on_channel_switch.sh /data/local/tmp/fuse_sensor /data/local/tmp/usb_tv.sh'" || true

    echo "[✓] Celular configurado: LAN=http://$(ip route get 8.8.8.8 | grep src | awk '{print $7}'):8080 | Tunnel=$TUNNEL_URL"
fi

echo ""
echo "=================================================="
echo "  TÚNEL ONLINE E PRONTO PARA A TV!"
echo "=================================================="
echo " [✓] Painel Web: $TUNNEL_URL"
echo " [✓] Stream URL: $STREAM_URL"
echo ""
echo " 📱 No Celular (Termux):"
echo "    Basta digitar: ./tv"
echo "    (Ou: ./tv start $STREAM_URL)"
echo ""
echo " Pressione Ctrl+C para encerrar o servidor."
echo "=================================================="

# Mantém o processo rodando
wait "$PID_SERVER"
