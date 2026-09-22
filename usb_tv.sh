#!/system/bin/sh
GADGET="/config/usb_gadget/g1"
[ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
UDC=$(getprop sys.usb.controller)
BACKING_IMG="/data/local/tmp/tv_stream.img"
PID_FILE="/data/local/tmp/channel_stream.pid"
LOG_FILE="/data/local/tmp/channel_stream.log"
SWITCH_SCRIPT="/data/local/tmp/on_channel_switch.sh"

MNT_POINT="/data/local/tmp/vfat_mnt"
FUSE_BIN="/data/local/tmp/fuse_sensor"
FUSE_LOG="/data/local/tmp/fuse_sensor.log"

stop_stream() {
  echo "[i] Parando processos anteriores..."
  echo "" > "$GADGET/UDC" 2>/dev/null || true
  WDPID=$(cat /data/local/tmp/watchdog.pid 2>/dev/null)
  [ -n "$WDPID" ] && kill -9 "$WDPID" 2>/dev/null
  rm -f /data/local/tmp/watchdog.pid
  pkill -9 -f "[f]use_direct" 2>/dev/null || true
  pkill -9 -f "[s]tream_writer.py" 2>/dev/null || true
  pkill -9 -f "[w]atch_writer.sh" 2>/dev/null || true
  umount -l "$MNT_POINT" 2>/dev/null || true
  rm -f /data/local/tmp/live_pipe 2>/dev/null || true
  if [ -f "$PID_FILE" ]; then
    kill -9 $(cat "$PID_FILE" 2>/dev/null) 2>/dev/null || true
    rm -f "$PID_FILE"
  fi
}

start_stream() {
  stop_stream

  echo "=================================================="
  echo "  Iniciando USB Stream TV (Modo FUSE Direct 24H)"
  echo "=================================================="

  # 1. Reset do Gadget UDC antes de reconfigurar
  echo "" > "$GADGET/UDC" 2>/dev/null || true
  sleep 1

  # 1b. Inicia FUSE Direct (Streaming Infinito em RAM) se disponível
  if [ -x /data/local/tmp/fuse_direct ] && [ -f /data/local/tmp/fat_template.bin ]; then
    echo "[+] Ativando FUSE Direct (Streaming Infinito em RAM)..."
    mkdir -p "$MNT_POINT"
    umount -l "$MNT_POINT" 2>/dev/null || true
    rm -f /data/local/tmp/live_pipe
    mkfifo /data/local/tmp/live_pipe
    setsid /data/local/tmp/fuse_direct "$MNT_POINT" /data/local/tmp/live_pipe /data/local/tmp/fat_template.bin >/data/local/tmp/fuse_direct.log 2>&1 &
    sleep 1
    BACKING_IMG="$MNT_POINT/tv_stream.img"
    echo "[✓] FUSE Direct ativo em $BACKING_IMG"
  else
    BACKING_IMG="/data/local/tmp/tv_stream.img"
  fi

  # 2. Configura a LUN 0 com a imagem (FUSE virtual ou disco)
  echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
  echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file"
  echo "[+] LUN Configurada: $(cat $GADGET/functions/mass_storage.0/lun.0/file)"
  echo 1 > "$GADGET/functions/mass_storage.0/lun.0/removable"
  echo 0 > "$GADGET/functions/mass_storage.0/lun.0/ro"
  echo "LIVETV" > "$GADGET/functions/mass_storage.0/lun.0/inquiry_string"
  rm -f "$GADGET/configs/b.1/f1" 2>/dev/null || true
  ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1"

  # 3. Sintoniza canal inicial (Preserva canal ativo ou usa globo-rj)
  CH=$(cat /data/local/tmp/current_channel.txt 2>/dev/null)
  [ -z "$CH" ] && CH="globo-rj"
  echo "[+] Sintonizando transmissão inicial (CANAL AO VIVO: $CH - Setor 3112)..."
  "$SWITCH_SCRIPT" "$CH" 3112

  # 3b. Cão de guarda: reinicia o gravador sozinho se ele morrer (LMK/crash)
  # (o script grava o próprio PID em watchdog.pid e se blinda do LMK)
  WDPID_OLD=$(cat /data/local/tmp/watchdog.pid 2>/dev/null)
  [ -n "$WDPID_OLD" ] && kill -9 "$WDPID_OLD" 2>/dev/null
  setsid /data/local/tmp/watch_writer.sh >/dev/null 2>&1 &
  sleep 1
  echo "[+] Watchdog ativo (PID $(cat /data/local/tmp/watchdog.pid 2>/dev/null))."

  # 3c. Inicia agente de telemetria se não estiver rodando
  if ! pgrep -f "[t]elemetry_agent.py" >/dev/null 2>&1; then
    setsid /data/data/com.termux/files/usr/bin/python3 /data/local/tmp/telemetry_agent.py >/dev/null 2>&1 &
    echo "[+] Agente de telemetria ativo."
  fi

  # 4. Ativa o controlador USB para a TV
  # Limita corrente de carga para 500mA para não derrubar a porta USB da TV
  echo 500000 > /sys/class/power_supply/usb/current_max 2>/dev/null || true
  echo "$UDC" > "$GADGET/UDC"

  echo "[+] Aguardando 15s para pré-encher o buffer de reprodução inicial (evita colisão)..."
  sleep 15
  echo "[✓] Buffer inicial pré-carregado com sucesso!"

  SERVER_PUBLIC=$(cat /data/local/tmp/server_url.txt 2>/dev/null | tr -d '\r\n')
  [ -z "$SERVER_PUBLIC" ] && SERVER_PUBLIC="http://192.168.1.8:8080"

  echo ""
  echo "=================================================="
  echo "  TRANSMISSÃO AO VIVO 24H CONECTADA NA TV!"
  echo "=================================================="
  echo "[✓] Pendrive virtual LIVETV conectado na TV."
  echo ""
  echo " 📺 Na sua TV Samsung Plasma:"
  echo " 1. Abra o menu USB (LIVETV) -> Vídeos"
  echo " 2. Abra o arquivo: [ TV AO VIVO.ts ]"
  echo " 3. Pressione PLAY!"
  echo " 4. Pressione Tools no controle remoto da TV -> 'Modo de Repetição' -> 'Repetir 1'"
  echo "    para reprodução 24 horas contínua sem parar!"
  echo ""
  echo " 📱 Para trocar de canal (pelo celular ou PC):"
  echo "    Acesse: $SERVER_PUBLIC"
  echo "    Basta clicar no canal desejado (Globo, Record News, Cultura, TNT, ESPN...)"
  echo "    A TV muda de canal instantaneamente sem fechar o vídeo!"
  echo "=================================================="
}

status_stream() {
  echo "=== STATUS DA TRANSMISSÃO ==="
  if pgrep -f "stream_writer.py" > /dev/null 2>&1; then
    echo "[✓] Gravador Contínuo: ATIVO (Rodando)"
  else
    echo "[-] Gravador: PARADO"
  fi
  if pgrep -f "fuse_direct" > /dev/null 2>&1; then
    echo "[✓] Motor FUSE Direct: ATIVO (Streaming em RAM)"
  elif pgrep -f "fuse_sensor" > /dev/null 2>&1; then
    echo "[✓] Sensor FUSE da TV: ATIVO (Monitorando seleção da TV)"
  else
    echo "[-] Motor FUSE: PARADO"
  fi

  echo "[+] LUN Conectada: $(cat $GADGET/functions/mass_storage.0/lun.0/file 2>/dev/null || echo nenhuma)"
  echo "[+] UDC: $(cat $GADGET/UDC 2>/dev/null || echo desconectado)"

  if [ -f "$LOG_FILE" ]; then
    echo ""
    echo "=== ÚLTIMOS LOGS DA TRANSMISSÃO ==="
    tail -n 12 "$LOG_FILE"
  fi
}

if echo "$1" | grep -qE "^https?://"; then
    URL_ARG="$1"
    ACTION="start"
elif echo "$2" | grep -qE "^https?://"; then
    URL_ARG="$2"
    ACTION="$1"
else
    ACTION="$1"
    URL_ARG=""
fi
[ -z "$ACTION" ] && ACTION="start"

if [ -n "$URL_ARG" ]; then
    echo "$URL_ARG" | sed 's|/live\.ts||' | tr -d '\r\n' > /data/local/tmp/server_url.txt
    echo "[+] URL do servidor atualizada para: $(cat /data/local/tmp/server_url.txt)"
fi

case "$ACTION" in
  start|live)
    start_stream
    ;;
  stop)
    stop_stream
    echo "[+] Desativando Mass Storage..."
    echo "none" > "$GADGET/UDC" 2>/dev/null || true
    sleep 1
    echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
    rm -f "$GADGET/configs/b.1/f1" 2>/dev/null || true
    ln -s "$GADGET/functions/mtp.gs0" "$GADGET/configs/b.1/f1" 2>/dev/null || true
    echo "$UDC" > "$GADGET/UDC"
    echo "[✓] Celular restaurado para o modo normal (MTP)."
    ;;
  status)
    status_stream
    ;;
  *)
    echo "Uso: ./tv [start|stop|status] [url_do_servidor]"
    ;;
esac
