#!/system/bin/sh
# watch_writer.sh — cão de guarda do stream_writer.py e do gadget USB Mass Storage
# Roda como root em loop a cada 5s:
# 1. Se o gravador morrer (LMK, crash, kill), reinicia no mesmo canal.
# 2. Se a TV for desligada/religada e o Android resetar o USB Gadget para MTP/Charging,
#    restaura o Mass Storage e re-conecta o UDC automaticamente!

PID_FILE=/data/local/tmp/channel_stream.pid
WD_PID_FILE=/data/local/tmp/watchdog.pid
LOG_FILE=/data/local/tmp/channel_stream.log
CH_FILE=/data/local/tmp/current_channel.txt
SEC_FILE=/data/local/tmp/current_sector.txt
URL_FILE=/data/local/tmp/server_url.txt
PYTHON=/data/data/com.termux/files/usr/bin/python3
GADGET="/config/usb_gadget/g1"
[ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
BACKING_IMG="/data/local/tmp/vfat_mnt/tv_stream.img"

echo $$ > "$WD_PID_FILE"
echo -1000 > /proc/$$/oom_score_adj 2>/dev/null || true

is_alive() {
    P=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$P" ] && kill -0 "$P" 2>/dev/null; then
        return 0
    fi
    if pgrep -f "[s]tream_writer.py" >/dev/null 2>&1; then
        return 0
    fi
    return 1
}

check_gadget() {
    [ ! -d "$GADGET" ] && return 0
    CURRENT_F1=$(readlink "$GADGET/configs/b.1/f1" 2>/dev/null)
    CURRENT_UDC=$(cat "$GADGET/UDC" 2>/dev/null)
    UDC_NAME=$(getprop sys.usb.controller)

    # Aceita tanto caminho absoluto quanto relativo (../../mass_storage.0)
    case "$CURRENT_F1" in
        *mass_storage.0) F1_OK=1 ;;
        *) F1_OK=0 ;;
    esac

    if [ "$F1_OK" -ne 1 ] || [ -z "$CURRENT_UDC" ]; then
        if [ -f "$BACKING_IMG" ]; then
            echo "[$(date '+%H:%M:%S')] watchdog: Gadget USB desconectado pelo Android. Restaurando LIVETV..." >> "$LOG_FILE"
            echo "" > "$GADGET/UDC" 2>/dev/null || true
            echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
            echo 1 > "$GADGET/functions/mass_storage.0/lun.0/removable" 2>/dev/null || true
            echo 0 > "$GADGET/functions/mass_storage.0/lun.0/ro" 2>/dev/null || true
            echo "LIVETV" > "$GADGET/functions/mass_storage.0/lun.0/inquiry_string" 2>/dev/null || true
            rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true
            ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1" 2>/dev/null || true
            [ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f2" 2>/dev/null || true
            echo 500000 > /sys/class/power_supply/usb/current_max 2>/dev/null || true
            echo "$UDC_NAME" > "$GADGET/UDC" 2>/dev/null || true
        fi
    fi
}

check_chisel() {
    if ! pgrep -f "[c]hisel" >/dev/null 2>&1; then
        if [ -x /data/local/tmp/chisel ]; then
            if [ ! -f /etc/resolv.conf ]; then
                mkdir -p /data/local/tmp/overlay_tmpfs
                mount -t tmpfs -o size=10M tmpfs /data/local/tmp/overlay_tmpfs 2>/dev/null || true
                mkdir -p /data/local/tmp/overlay_tmpfs/upper /data/local/tmp/overlay_tmpfs/work 2>/dev/null || true
                echo "nameserver 8.8.8.8" > /data/local/tmp/overlay_tmpfs/upper/resolv.conf 2>/dev/null || true
                echo "nameserver 1.1.1.1" >> /data/local/tmp/overlay_tmpfs/upper/resolv.conf 2>/dev/null || true
                chmod 644 /data/local/tmp/overlay_tmpfs/upper/resolv.conf 2>/dev/null || true
                mount -t overlay -o lowerdir=/system/etc,upperdir=/data/local/tmp/overlay_tmpfs/upper,workdir=/data/local/tmp/overlay_tmpfs/work overlay /system/etc 2>/dev/null || true
            fi
            nohup /data/local/tmp/chisel client --keepalive 15s --auth tablet:tvbridge2026 --header "Host: tv.smre.run.place" http://129.146.5.64:80/chisel R:25556:127.0.0.1:5555 R:0.0.0.0:1080:socks >> /data/local/tmp/chisel.log 2>&1 &
        fi
    fi
}

check_adb() {
    if ! grep -q " 00000000:15B3 " /proc/net/tcp 2>/dev/null && ! grep -q " 00000000000000000000000000000000:15B3 " /proc/net/tcp6 2>/dev/null; then
        setprop persist.adb.tcp.port 5555
        setprop service.adb.tcp.port 5555
    fi
}

TICK=0
while true; do
    sleep 5
    check_gadget
    check_chisel
    check_adb
    TICK=$((TICK + 1))
    if [ $TICK -ge 3 ]; then
        TICK=0
        if [ ! -f /data/local/tmp/vod_mode.flag ] && ! is_alive; then
            CH=""
            TUNNEL_URL=$(cat "$URL_FILE" 2>/dev/null | tr -d '\r\n')
            for CAND in "$TUNNEL_URL" "http://127.0.0.1:8080" "http://192.168.1.5:8080" "http://192.168.1.8:8080"; do
                [ -z "$CAND" ] && continue
                if [ -x "/data/local/tmp/curl" ]; then
                    CH=$(/data/local/tmp/curl -s --max-time 3 "$CAND/api/status" 2>/dev/null | grep -o '"active_channel_id": *"[^"]*"' | head -n1 | cut -d'"' -f4)
                elif [ -x "$PYTHON" ]; then
                    CH=$("$PYTHON" -c "import urllib.request,json,sys; print(json.load(urllib.request.urlopen('$CAND/api/status', timeout=3))['active_channel_id'])" 2>/dev/null)
                fi
                [ -n "$CH" ] && break
            done
            [ -z "$CH" ] && CH=$(cat "$CH_FILE" 2>/dev/null)
            [ -z "$CH" ] && CH="globo-morena-dourados"
            SEC=$(cat "$SEC_FILE" 2>/dev/null); [ -z "$SEC" ] && SEC="3112"
            echo "[$(date '+%H:%M:%S')] watchdog: writer morto, reiniciando no canal ativo ($CH)" >> "$LOG_FILE"
            /data/local/tmp/on_channel_switch.sh "$CH" "$SEC"
        fi
    fi
done
