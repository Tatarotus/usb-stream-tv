#!/system/bin/sh
CHANNEL_ID="$1"
START_SECTOR="$2"
URL_FILE="/data/local/tmp/server_url.txt"
PID_FILE="/data/local/tmp/channel_stream.pid"
LOG_FILE="/data/local/tmp/channel_stream.log"

export PATH=/data/data/com.termux/files/usr/bin:$PATH
export LD_LIBRARY_PATH=/data/data/com.termux/files/usr/lib
PYTHON=/data/data/com.termux/files/usr/bin/python3

[ -z "$CHANNEL_ID" ] && exit 0
[ -z "$START_SECTOR" ] && START_SECTOR=3112

# Persiste o canal atual para o watchdog retomar após morte/reboot
echo "$CHANNEL_ID" > /data/local/tmp/current_channel.txt
echo "$START_SECTOR" > /data/local/tmp/current_sector.txt

# 1. Detecta URL do servidor (LAN primeiro, depois tunnel)
# Sem curl no aparelho: sonda LAN via python (urllib, timeout 2s)
# Dois IPs candidatos: .5 = Wi-Fi do servidor, .8 = Ethernet/USB do servidor (DHCP muda)
TUNNEL_URL=""
[ -f "$URL_FILE" ] && TUNNEL_URL=$(cat "$URL_FILE" 2>/dev/null | tr -d '\r\n')

# Prioritize Cloudflare Tunnel so stream continues seamlessly on the TV
SERVER_URL=""
if [ -n "$TUNNEL_URL" ] && "$PYTHON" -c "import urllib.request; urllib.request.urlopen('$TUNNEL_URL/api/status', timeout=3).read()" >/dev/null 2>&1; then
    SERVER_URL="$TUNNEL_URL"
else
    LAN_URLS="http://192.168.1.8:8080 http://127.0.0.1:8080 http://192.168.1.5:8080"
    for CAND in $LAN_URLS; do
      if "$PYTHON" -c "import urllib.request; urllib.request.urlopen('$CAND/api/status', timeout=2).read()" >/dev/null 2>&1; then
        SERVER_URL="$CAND"
        break
      fi
    done
fi
[ -z "$SERVER_URL" ] && SERVER_URL="${TUNNEL_URL:-http://127.0.0.1:8080}"

# 2. Mata o gravador anterior de forma limpa
# (colchetes evitam que o pkill mate o próprio shell deste script)
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE" 2>/dev/null)
    [ -n "$OLD_PID" ] && kill -9 "$OLD_PID" 2>/dev/null
    rm -f "$PID_FILE"
fi
pkill -9 -f "[s]tream_writer.py" 2>/dev/null || true
sleep 0.1

# 3. Inicia o novo gravador circular em Python (ou modo FIFO se FUSE ativo)
FIFO_OPT=""
[ -p /data/local/tmp/live_pipe ] && FIFO_OPT="--fifo=/data/local/tmp/live_pipe"
$PYTHON /data/local/tmp/stream_writer.py "$START_SECTOR" "$SERVER_URL" "$CHANNEL_ID" $FIFO_OPT >> "$LOG_FILE" 2>&1 &
WPID=$!
# Protege contra o LMK (low-memory killer) do Android
echo -900 > /proc/$WPID/oom_score_adj 2>/dev/null || true
