#!/system/bin/sh
# usb_tv.sh - Controle mestre do USB Stream TV no celular (Mi A2)
# Suporta: ./tv start [URL], ./tv stop, ./tv restart, ./tv status

# 0. Auto-elevação para Root se executado no Termux sem su
if [ "$(id -u)" != "0" ]; then
  exec su -c "sh $0 $@"
fi

GADGET="/config/usb_gadget/g1"
[ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
UDC=$(getprop sys.usb.controller)
MNT_POINT="/data/local/tmp/vfat_mnt"
LOG_FILE="/data/local/tmp/channel_stream.log"
URL_FILE="/data/local/tmp/server_url.txt"
PYTHON="/data/data/com.termux/files/usr/bin/python3"
[ ! -x "$PYTHON" ] && PYTHON="python3"

stop_stream() {
  echo "[i] Parando processos e desconectando da TV..."
  
  # 1. Desconecta o controlador USB da TV
  echo "" > "$GADGET/UDC" 2>/dev/null || true
  echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
  rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true

  # 2. Encerra processos
  pkill -9 -f "[w]atch_writer.sh" 2>/dev/null || true
  pkill -9 -f "[s]tream_writer.py" 2>/dev/null || true
  pkill -9 -f "[f]use_direct" 2>/dev/null || true
  rm -f /data/local/tmp/watchdog.pid /data/local/tmp/channel_stream.pid 2>/dev/null || true

  # 3. Desmonta o sistema de arquivos FUSE
  umount -l "$MNT_POINT" 2>/dev/null || true
  rm -f /data/local/tmp/live_pipe /data/local/tmp/stream_abspos.txt /data/local/tmp/writer_pos.txt 2>/dev/null || true

  # 4. Restaura ADB se a função existir
  if [ -d "$GADGET/functions/ffs.adb" ]; then
    ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f1" 2>/dev/null || true
    echo "$UDC" > "$GADGET/UDC" 2>/dev/null || true
  fi

  echo "[✓] USB Stream TV parado e pendrive desmontado com sucesso!"
}

start_stream() {
  stop_stream

  echo "=================================================="
  echo "  Iniciando USB Stream TV (Modo FUSE Direct 24H)  "
  echo "=================================================="

  # 1. Garante que UDC está limpo
  echo "" > "$GADGET/UDC" 2>/dev/null || true
  sleep 1

  # 2. Inicia o motor FUSE Direct em RAM
  if [ -x /data/local/tmp/fuse_direct ] && [ -f /data/local/tmp/fat_template.bin ]; then
    echo "[+] Ativando FUSE Direct (Streaming Infinito em RAM)..."
    mkdir -p "$MNT_POINT"
    umount -l "$MNT_POINT" 2>/dev/null || true
    rm -f /data/local/tmp/live_pipe
    mkfifo /data/local/tmp/live_pipe
    nohup /data/local/tmp/fuse_direct "$MNT_POINT" /data/local/tmp/live_pipe /data/local/tmp/fat_template.bin >/data/local/tmp/fuse_direct.log 2>&1 &
    sleep 1
    BACKING_IMG="$MNT_POINT/tv_stream.img"
    echo "[✓] FUSE Direct ativo em $BACKING_IMG"
  else
    echo "[!] ERRO: /data/local/tmp/fuse_direct ou fat_template.bin não encontrados!"
    return 1
  fi

  # 3. Configura o gadget USB Mass Storage (LUN 0)
  echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file"
  echo 1 > "$GADGET/functions/mass_storage.0/lun.0/removable"
  echo 0 > "$GADGET/functions/mass_storage.0/lun.0/ro"
  echo "LIVETV" > "$GADGET/functions/mass_storage.0/lun.0/inquiry_string"
  rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true
  ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1"
  [ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f2" 2>/dev/null || true

  # 4. Determina URL do servidor e canal
  SERVER_URL="https://tv.smre.run.place"
  if [ -f "$URL_FILE" ]; then
    CAND_URL=$(cat "$URL_FILE" 2>/dev/null | tr -d '\r\n')
    [ -n "$CAND_URL" ] && SERVER_URL="$CAND_URL"
  fi

  CH=$(cat /data/local/tmp/current_channel.txt 2>/dev/null)
  [ -z "$CH" ] && CH="band-rio"

  echo "[+] Iniciando gravador: $SERVER_URL (Canal: $CH)..."
  nohup $PYTHON /data/local/tmp/stream_writer.py 3112 "$SERVER_URL" "$CH" --fifo=/data/local/tmp/live_pipe >>/data/local/tmp/channel_stream.log 2>&1 &
  nohup sh /data/local/tmp/watch_writer.sh >>/data/local/tmp/channel_stream.log 2>&1 &

  # 5. Mantém telemetria ativa
  if ! pgrep -f "[t]elemetry_agent.py" >/dev/null 2>&1; then
    nohup $PYTHON /data/local/tmp/telemetry_agent.py >/dev/null 2>&1 &
    echo "[+] Agente de telemetria ativo."
  fi

  # 6. Limita corrente para 500mA (seguro para porta USB de TV)
  echo 500000 > /sys/class/power_supply/usb/current_max 2>/dev/null || true

  # 7. Pré-carregamento do buffer inicial (regra áurea de 15 segundos)
  echo "[+] Aguardando 15s para pré-encher o buffer de reprodução inicial (evita colisão)..."
  sleep 15
  echo "[✓] Buffer inicial pré-carregado com sucesso!"

  # 8. Conecta o USB na TV
  echo "$UDC" > "$GADGET/UDC"
  echo "[✓] Pendrive virtual LIVETV conectado na TV (UDC=$UDC)!"

  echo ""
  echo "=================================================="
  echo "  TRANSMISSÃO AO VIVO 24H CONECTADA NA TV!"
  echo "=================================================="
  echo " 📺 Na sua TV Samsung:"
  echo " 1. Abra o menu USB (LIVETV) -> Vídeos"
  echo " 2. Abra o arquivo: [ TV AO VIVO.ts ] e aperte PLAY"
  echo " 3. Aperte Tools no controle remoto -> 'Modo de Repetição' -> 'Repetir 1'"
  echo "=================================================="
}

status_stream() {
  echo "=== STATUS DA TRANSMISSÃO (Mi A2) ==="
  if pgrep -f "stream_writer.py" > /dev/null 2>&1; then
    echo "[✓] Gravador Contínuo: ATIVO"
  else
    echo "[-] Gravador: PARADO"
  fi
  if pgrep -f "fuse_direct" > /dev/null 2>&1; then
    echo "[✓] Motor FUSE Direct: ATIVO"
  else
    echo "[-] Motor FUSE: PARADO"
  fi
  if pgrep -f "watch_writer.sh" > /dev/null 2>&1; then
    echo "[✓] Watchdog do Gravador: ATIVO"
  else
    echo "[-] Watchdog: PARADO"
  fi

  echo "[+] LUN Conectada: $(cat $GADGET/functions/mass_storage.0/lun.0/file 2>/dev/null || echo nenhuma)"
  echo "[+] UDC: $(cat $GADGET/UDC 2>/dev/null || echo desconectado)"

  if [ -f "/data/local/tmp/fuse_direct.log" ]; then
    echo ""
    echo "=== ÚLTIMAS LEITURAS FUSE (TV) ==="
    tail -n 5 /data/local/tmp/fuse_direct.log
  fi
}

# Processamento de argumentos
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
    echo "$URL_ARG" | sed 's|/live\.ts||' | tr -d '\r\n' > "$URL_FILE"
    echo "[+] URL do servidor atualizada para: $(cat $URL_FILE)"
fi

case "$ACTION" in
  start|live)
    start_stream
    ;;
  stop)
    stop_stream
    ;;
  status)
    status_stream
    ;;
  restart)
    stop_stream
    sleep 2
    start_stream
    ;;
  *)
    echo "Uso: ./tv [start|stop|restart|status] [url_do_servidor]"
    ;;
esac
