#!/system/xbin/busybox sh
# switch_tv_mode.sh — Alternador entre Modo TV Ao Vivo e Modo Cinema (VOD)
# Samsung Galaxy Tab 3 Lite (SM-T110)
trap '' HUP

LOCK_FILE="/data/local/tmp/mode_switch.lock"
if [ -f "$LOCK_FILE" ]; then
    echo "[!] Troca de modo ja em andamento (lock ativo). Aguarde..."
    exit 0
fi
touch "$LOCK_FILE"
trap 'rm -f "$LOCK_FILE"' EXIT INT TERM HUP

MODE="$1"
MNT="/data/local/tmp/ntfs_lab/mnt"
FIFO="/data/local/tmp/live_pipe"
LIVE_TMPL="/data/local/tmp/ntfs_lab/ntfs_template.bin"
VOD_TMPL="/data/local/tmp/ntfs_lab/vod_template.bin"
FAV_TMPL="/data/local/tmp/ntfs_lab/favorites_template.bin"
FUSE_BIN="/system/xbin/fuse_ntfs"
BACKING_IMG="$MNT/tv_stream.img"
FLAG_FILE="/data/local/tmp/vod_mode.flag"
VOD_FLAG="$FLAG_FILE"
FAV_FLAG="/data/local/tmp/favorites_mode.flag"
LOG="/data/local/tmp/ntfs_lab/fuse_ntfs.log"

# Garantir performance e wakelock sempre
echo "tv_stream" > /sys/power/wake_lock 2>/dev/null || true
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
done
/system/xbin/iwconfig wlan0 power off 2>/dev/null || true

cleanup_stack() {
    # 1. Desconectar LUN para que o kernel libere o backing file
    echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true

    # 2. Matar processos anteriores de forma direta
    killall -9 stream_fetcher fuse_ntfs fuse_direct fuse_direct_arm32 2>/dev/null || true

    # 3. Desmontar todas as camadas FUSE sobrepostas
    while /system/xbin/busybox mount | grep -q "$MNT"; do
        /system/xbin/busybox umount -l "$MNT" 2>/dev/null || break
    done
    while /system/xbin/busybox mount | grep -q "/data/local/tmp/vfat_mnt"; do
        /system/xbin/busybox umount -l "/data/local/tmp/vfat_mnt" 2>/dev/null || break
    done
    sleep 1
}

case "$MODE" in
    vod|cinema)
        echo "[*] ========================================="
        echo "[*]       ATIVANDO MODO CINEMA (VOD)         "
        echo "[*] ========================================="
        rm -f "$FAV_FLAG"
        touch "$FLAG_FILE"

        cleanup_stack

        # Iniciar FUSE no modo hierarquico VOD (--multi)
        mkdir -p "$MNT"
        /system/xbin/busybox setsid "$FUSE_BIN" "$MNT" "test" "$VOD_TMPL" --multi > /data/local/tmp/ntfs_lab/fuse_ntfs.log 2>&1 &
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -f "$BACKING_IMG" ] && break
            sleep 1
        done

        if [ ! -f "$BACKING_IMG" ]; then
            echo "[!] ERRO: Disco virtual VOD nao montou!"
            cat /data/local/tmp/ntfs_lab/fuse_ntfs.log
            exit 1
        fi

        # Reanexar LUN USB com identidade CINEMA
        echo "CINEMA" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
        echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
        echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true

        echo "[✓] Modo Cinema VOD ativo com sucesso na TV!"
        ;;

    live|tv)
        echo "[*] ========================================="
        echo "[*]      RETORNANDO PARA TV AO VIVO          "
        echo "[*] ========================================="
        rm -f "$FLAG_FILE" /data/local/tmp/favorites_mode.flag

        cleanup_stack
        rm -f "$LOG" /data/local/tmp/ntfs_lab/stream_fetcher.log

        # Recriar pipe limpo
        rm -f "$FIFO" /data/local/tmp/fetcher_heartbeat.ts /data/local/tmp/stream_fetcher.lock 2>/dev/null || true
        mkfifo "$FIFO" 2>/dev/null || true
        chmod 666 "$FIFO" 2>/dev/null || true
        chmod 666 /dev/fuse 2>/dev/null || true


        # Alternar nome do arquivo NTFS (TV AO VIVO.trp <-> TV AO VIVO 2.tp) para invalidar cache da TV
        if [ -x "/data/local/tmp/patch_trp" ] && [ -f "$LIVE_TMPL" ]; then
            /data/local/tmp/patch_trp "$LIVE_TMPL" >/dev/null 2>&1 || true
        fi

        # Iniciar FUSE no modo Live TV padrao
        mkdir -p "$MNT"
        "$FUSE_BIN" "$MNT" "$FIFO" "$LIVE_TMPL" > "$LOG" 2>&1 &
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -f "$BACKING_IMG" ] && break
            sleep 1
        done

        if [ ! -f "$BACKING_IMG" ]; then
            echo "[!] ERRO: Disco virtual Live TV nao montou!"
            cat "$LOG" 2>/dev/null || true
            exit 1
        fi

        # Iniciar exatamente 1 stream_fetcher da Live TV
        /system/xbin/stream_fetcher "$FIFO" tv.smre.run.place 80 > /data/local/tmp/ntfs_lab/stream_fetcher.log 2>&1 &

        # Aguardar pre-buffering de seguranca (>= 10MB) antes de expor LUN para a TV
        echo "[*] Aguardando pre-buffering de seguranca (>= 10MB)..."
        for i in $(seq 1 35); do
            if [ -f "$LOG" ]; then
                MB=$(grep -o "S_write=[0-9]*MB" "$LOG" | tail -n 1 | sed 's/[^0-9]//g')
                if [ -n "$MB" ] && [ "$MB" -ge 10 ]; then
                    echo "[*] Pre-buffer OK: ${MB}MB >= 10MB"
                    break
                fi
            fi
            sleep 1
        done

        # Reanexar LUN USB com alternancia LIVETV1/LIVETV2
        CUR_INQ=$(cat /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null)
        if [ "$CUR_INQ" = "LIVETV1" ]; then
            NEW_INQ="LIVETV2"
        else
            NEW_INQ="LIVETV1"
        fi
        echo "$NEW_INQ" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
        echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 0 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
        echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true

        echo "[✓] TV Ao Vivo restaurada com sucesso na TV ($NEW_INQ)!"
        ;;

    favorites|channels)
        echo "[*] ========================================="
        echo "[*] Switching to FAVORITES MULTI-CHANNEL Live NTFS mode..."
        echo "[*] ========================================="
        # Flag to indicate favorites mode
        rm -f "$VOD_FLAG"
        touch /data/local/tmp/favorites_mode.flag
        cleanup_stack

        # Ensure FIFO exists
        rm -f "$FIFO" /data/local/tmp/fetcher_heartbeat.ts /data/local/tmp/stream_fetcher.lock
        mkfifo "$FIFO"
        chmod 666 "$FIFO" /dev/fuse 2>/dev/null

        # Mount fuse_ntfs with favorites template
        FAV_TMPL="/data/local/tmp/ntfs_lab/favorites_template.bin"
        if [ ! -f "$FAV_TMPL" ]; then
            echo "[!] Favorites template not found at $FAV_TMPL"
            exit 1
        fi

        mkdir -p "$MNT"
        echo "[*] Launching fuse_ntfs with $FAV_TMPL..."
        /system/xbin/fuse_ntfs "$MNT" "$FIFO" "$FAV_TMPL" > "$LOG" 2>&1 &
        FUSE_PID=$!
        echo "[*] fuse_ntfs started with PID $FUSE_PID"

        # Wait for mount
        # Wait up to 10s for backing file
        for i in 1 2 3 4 5 6 7 8 9 10; do
            [ -f "$BACKING_IMG" ] && break
            sleep 1
        done

        if [ ! -f "$BACKING_IMG" ]; then
            echo "[!] ERRO: Disco virtual Favorites nao montou!"
            cat "$LOG" 2>/dev/null || true
            exit 1
        fi

        # Launch stream_fetcher
        /system/xbin/stream_fetcher "$FIFO" tv.smre.run.place 80 > /data/local/tmp/ntfs_lab/stream_fetcher.log 2>&1 &

        # Wait for prebuffer (16MB)
        echo "[*] Aguardando pre-buffering de seguranca (>= 16MB)..."
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
            if [ -f "$LOG" ]; then
                MB=$(grep -o "S_write=[0-9]*MB" "$LOG" | tail -n 1 | sed 's/[^0-9]//g')
                if [ -n "$MB" ] && [ "$MB" -ge 16 ]; then
                    echo "[*] Pre-buffer OK: ${MB}MB >= 16MB"
                    break
                fi
            fi
            sleep 1
        done

        # USB Gadget bind with inquiry_string "CHANNELS"
        echo "CHANNELS" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
        echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 0 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
        echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
        echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true

        echo "[✓] Modo Favoritos / Multi-Canais ativo com sucesso na TV!"
        ;;

    status)
        echo "=== STATUS DO MODO TV ==="
        if [ -f "$FAV_FLAG" ]; then
            echo "[*] Modo ativo: FAVORITOS / MULTI-CANAIS ($FAV_FLAG presente)"
        elif [ -f "$FLAG_FILE" ]; then
            echo "[*] Modo ativo: CINEMA / VOD ($FLAG_FILE presente)"
        else
            echo "[*] Modo ativo: TV AO VIVO (PADRAO)"
        fi

        INQ=$(cat /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null)
        LUN_FILE=$(cat /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null)
        LUN_EN=$(cat /sys/class/android_usb/android0/enable 2>/dev/null)
        echo "[*] USB Inquiry: ${INQ:-N/A}"
        echo "[*] Backing File: ${LUN_FILE:-nenhum}"
        echo "[*] USB Enable: ${LUN_EN:-0}"

        if pidof fuse_ntfs >/dev/null 2>&1 || killall -0 fuse_ntfs 2>/dev/null; then
            echo "[*] Processo FUSE: ATIVO"
        else
            echo "[*] Processo FUSE: PARADO"
        fi

        if pidof stream_fetcher >/dev/null 2>&1 || killall -0 stream_fetcher 2>/dev/null; then
            echo "[*] Processo Fetcher: ATIVO"
        else
            echo "[*] Processo Fetcher: PARADO"
        fi
        ;;

    *)
        echo "Uso: $0 {vod|cinema|live|tv|favorites|channels|status}"
        exit 1
        ;;
esac
