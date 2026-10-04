#!/system/bin/sh
# rollback_vod.sh — One-Command Instant Rollback for VOD
# Samsung Galaxy Tab 3 Lite (SM-T110) & Xiaomi Mi A2

# Portable Busybox resolver & system shell fallbacks
BB=""
if [ -x "/system/xbin/busybox" ]; then
    BB="/system/xbin/busybox"
elif [ -x "/data/local/tmp/busybox" ]; then
    BB="/data/local/tmp/busybox"
elif command -v busybox >/dev/null 2>&1; then
    BB="$(command -v busybox)"
fi

if [ -n "$BB" ] && [ -z "$BB_ACTIVE" ]; then
    export BB_ACTIVE=1
    exec "$BB" sh "$0" "$@"
fi

set -e

LOCAL_DIR="/data/local/tmp"
MNT_POINT="$LOCAL_DIR/vfat_mnt"
BACKING_IMG="$MNT_POINT/tv_stream.img"
FLAG_FILE="$LOCAL_DIR/vod_mode.flag"
TMPL_LOCAL="$LOCAL_DIR/fat_template_vod.bin"
V2_BIN="$LOCAL_DIR/fuse_direct_arm32_v2"
BAK_BIN="$LOCAL_DIR/fuse_direct_arm32.bak_orig"
ORIG_BIN="$LOCAL_DIR/fuse_direct_arm32"
LOG_FILE="$LOCAL_DIR/fuse_vod.log"
LOCK_DIR="$LOCAL_DIR/mode_switch.lock.dir"
LOCK_OWNED=0

# Helper wrappers for Portable Busybox / standard shell fallback
bb_chmod() {
    if [ -n "$BB" ]; then
        "$BB" chmod "$@" 2>/dev/null || chmod "$@" 2>/dev/null || true
    else
        chmod "$@" 2>/dev/null || true
    fi
}

bb_filesize() {
    _file="$1"
    _size=""
    if [ -n "$BB" ]; then
        _size=$("$BB" stat -c %s "$_file" 2>/dev/null || true)
    fi
    if [ -z "$_size" ] && command -v stat >/dev/null 2>&1; then
        _size=$(stat -c %s "$_file" 2>/dev/null || true)
    fi
    if [ -z "$_size" ]; then
        _size=$(wc -c < "$_file" 2>/dev/null | tr -d ' ' || echo 0)
    fi
    echo "${_size:-0}"
}

bb_mtime() {
    _file="$1"
    _m=""
    if [ -n "$BB" ]; then
        _m=$("$BB" stat -c %Y "$_file" 2>/dev/null || true)
    fi
    if [ -z "$_m" ] && command -v stat >/dev/null 2>&1; then
        _m=$(stat -c %Y "$_file" 2>/dev/null || true)
    fi
    echo "${_m:-0}"
}

bb_pgrep() {
    _pattern="$1"
    _pids=""
    if [ -n "$BB" ]; then
        _pids=$("$BB" pgrep -f "$_pattern" 2>/dev/null || true)
    fi
    if [ -z "$_pids" ] && command -v pgrep >/dev/null 2>&1; then
        _pids=$(pgrep -f "$_pattern" 2>/dev/null || true)
    fi
    if [ -z "$_pids" ]; then
        # Fallback usando ps padrao
        _pids=$(ps 2>/dev/null | while read -r _u _p _rest; do
            case "$_p" in
                *[!0-9]*) continue ;;
            esac
            case "$_rest" in
                *"$_pattern"*) echo "$_p" ;;
            esac
        done)
    fi
    echo "$_pids"
}

bb_kill_pattern() {
    _pat="$1"
    _sig="${2:-15}"
    for _pid in $(bb_pgrep "$_pat"); do
        [ -n "$_pid" ] && kill "-$_sig" "$_pid" 2>/dev/null || true
    done
}

bb_mount_grep() {
    _target="$1"
    if [ -n "$BB" ]; then
        "$BB" mount 2>/dev/null | grep -q "$_target" && return 0
    fi
    mount 2>/dev/null | grep -q "$_target" && return 0
    return 1
}

bb_umount() {
    _target="$1"
    if [ -n "$BB" ]; then
        "$BB" umount -l "$_target" 2>/dev/null || umount -l "$_target" 2>/dev/null || true
    else
        umount -l "$_target" 2>/dev/null || umount "$_target" 2>/dev/null || true
    fi
}

bb_setsid() {
    if [ -n "$BB" ]; then
        "$BB" setsid "$@"
    elif command -v setsid >/dev/null 2>&1; then
        setsid "$@"
    else
        ( "$@" )
    fi
}

is_valid_elf() {
    _file="$1"
    [ -f "$_file" ] || return 1
    _sz=$(bb_filesize "$_file")
    [ "$_sz" -gt 102400 ] || return 1

    # Finding 3: Validacao rigorosa dos 4 bytes magicos ELF: 0x7F 'E' 'L' 'F' (7f454c46)
    head -c 4 "$_file" 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n' | grep -q "7f454c46" && return 0
    dd if="$_file" bs=1 count=4 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n' | grep -q "7f454c46" && return 0

    # Fallback busybox para tablet Android (caso head/od nao estejam no PATH direto)
    if [ -n "$BB" ]; then
        "$BB" head -c 4 "$_file" 2>/dev/null | "$BB" od -An -tx1 2>/dev/null | "$BB" tr -d ' \n' | "$BB" grep -q "7f454c46" && return 0
        "$BB" dd if="$_file" bs=1 count=4 2>/dev/null | "$BB" od -An -tx1 2>/dev/null | "$BB" tr -d ' \n' | "$BB" grep -q "7f454c46" && return 0
        "$BB" hexdump -n 4 -e '4/1 "%02x"' "$_file" 2>/dev/null | "$BB" grep -q "7f454c46" && return 0
    fi

    if command -v hexdump >/dev/null 2>&1; then
        hexdump -n 4 -e '4/1 "%02x"' "$_file" 2>/dev/null | grep -q "7f454c46" && return 0
    fi

    return 1
}

# Finding 12: Atomic Directory Lock with 30s Stale Detection & Strict Ownership
acquire_lock() {
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        LOCK_OWNED=1
    else
        echo "[!] AVISO: Lock existente detectado em $LOCK_DIR."
        _now=$(date +%s 2>/dev/null || echo 0)
        _ts=$(cat "$LOCK_DIR/ts" 2>/dev/null | tr -cd '0-9')
        _pid=$(cat "$LOCK_DIR/pid" 2>/dev/null | tr -cd '0-9')
        _stale=0

        # Se o processo que criou o lock ja morreu, lock eh orfao (stale)
        if [ -n "$_pid" ] && ! kill -0 "$_pid" 2>/dev/null; then
            _stale=1
            echo "[*] Processo PID $_pid nao esta mais ativo (lock orfao stale)."
        fi

        # Se tiver mais de 30 segundos, eh considerado stale
        if [ "$_stale" -eq 0 ] && [ -n "$_ts" ] && [ "$_now" -gt 0 ] && [ "$_ts" -gt 0 ]; then
            _age=$((_now - _ts))
            if [ "$_age" -ge 30 ]; then
                _stale=1
                echo "[*] Lock stale detectado por tempo (idade: ${_age}s >= 30s)."
            else
                echo "[!] Lock ativo e recente (idade: ${_age}s < 30s). Operacao concorrente em andamento."
                echo "[!] Abortando para proteger a transicao de modo da TV."
                exit 1
            fi
        elif [ "$_stale" -eq 0 ]; then
            _mtime=$(bb_mtime "$LOCK_DIR")
            if [ "$_mtime" -gt 0 ] && [ "$_now" -gt 0 ]; then
                _age=$((_now - _mtime))
                if [ "$_age" -ge 30 ]; then
                    _stale=1
                    echo "[*] Lock stale detectado por mtime (idade: ${_age}s >= 30s)."
                else
                    echo "[!] Lock ativo e recente por mtime (idade: ${_age}s < 30s). Abortando."
                    exit 1
                fi
            else
                echo "[!] Lock concorrente detectado em $LOCK_DIR. Abortando com seguranca."
                exit 1
            fi
        fi

        if [ "$_stale" -eq 1 ]; then
            echo "[*] Limpando lock stale ($LOCK_DIR)..."
            rmdir "$LOCK_DIR" 2>/dev/null || rm -rf "$LOCK_DIR" 2>/dev/null || true
            sleep 1
            if ! mkdir "$LOCK_DIR" 2>/dev/null; then
                echo "[!] ERRO FATAL: Falha ao adquirir lock apos limpar $LOCK_DIR!"
                exit 1
            fi
            LOCK_OWNED=1
        fi
    fi

    # Registrar PID e timestamp no lock dir adquirido
    date +%s > "$LOCK_DIR/ts" 2>/dev/null || true
    echo "$$" > "$LOCK_DIR/pid" 2>/dev/null || true
}

release_lock() {
    [ "${LOCK_OWNED:-0}" -eq 1 ] || return 0
    if [ ! -d "$LOCK_DIR" ]; then
        LOCK_OWNED=0
        return 0
    fi

    if [ -f "$LOCK_DIR/pid" ]; then
        _lock_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null | tr -cd '0-9')
        if [ "$_lock_pid" != "$$" ]; then
            return 0
        fi
    fi

    rm -rf "$LOCK_DIR" 2>/dev/null || rmdir "$LOCK_DIR" 2>/dev/null || true
    rm -f "$LOCAL_DIR/mode_switch.lock" 2>/dev/null || true
    LOCK_OWNED=0
}

cleanup_and_exit() {
    _code=$?
    release_lock
    exit "$_code"
}

trap 'cleanup_and_exit' EXIT INT TERM HUP

# Argument parsing fix: Separar host de task ID para evitar sobrescrita
SERVER_HOST=""
TASK_ID=""

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--host)
            SERVER_HOST="$2"
            shift 2
            ;;
        --host=*)
            SERVER_HOST="${1#*=}"
            shift
            ;;
        -t|--task|--task-id)
            TASK_ID="$2"
            shift 2
            ;;
        --task=*|--task-id=*)
            TASK_ID="${1#*=}"
            shift
            ;;
        *)
            if [ -z "$TASK_ID" ] && [ -z "$SERVER_HOST" ]; then
                case "$1" in
                    *.*|*:*|localhost)
                        SERVER_HOST="$1"
                        ;;
                    *)
                        TASK_ID="$1"
                        ;;
                esac
            elif [ -z "$SERVER_HOST" ]; then
                SERVER_HOST="$1"
            elif [ -z "$TASK_ID" ]; then
                TASK_ID="$1"
            fi
            shift
            ;;
    esac
done

[ -z "$SERVER_HOST" ] && SERVER_HOST="tv.smre.run.place"

restore_live_mode() {
    echo "[*] Tentando restaurar Modo TV Ao Vivo..."
    bb_kill_pattern "fuse_direct" 15
    sleep 1
    bb_kill_pattern "fuse_direct" 9
    _u_count=0
    while bb_mount_grep "$MNT_POINT"; do
        bb_umount "$MNT_POINT"
        sleep 0.5
        _u_count=$((_u_count + 1))
        if [ "$_u_count" -ge 10 ]; then
            echo "[!] Aviso: Atingido limite de 10 tentativas de umount..."
            break
        fi
    done
    rm -f "$FLAG_FILE" 2>/dev/null || true

    # Restaurar binario original se backup integro existir
    if [ -f "$BAK_BIN" ] && [ "$BAK_BIN" != "$ORIG_BIN" ] && is_valid_elf "$BAK_BIN"; then
        echo "[*] Restaurando $BAK_BIN -> $ORIG_BIN..."
        cp -p "$BAK_BIN" "$ORIG_BIN" 2>/dev/null || cp "$BAK_BIN" "$ORIG_BIN"
        bb_chmod 755 "$ORIG_BIN"
    fi

    RESTORATION_OK=0
    if [ -f "$LOCAL_DIR/switch_live.sh" ]; then
        echo "[*] Executando sh $LOCAL_DIR/switch_live.sh..."
        sh "$LOCAL_DIR/switch_live.sh" && RESTORATION_OK=1
    elif [ -x "/system/xbin/switch_live.sh" ]; then
        echo "[*] Executando /system/xbin/switch_live.sh..."
        /system/xbin/switch_live.sh && RESTORATION_OK=1
    elif [ -x "/system/xbin/switch_tv_mode.sh" ]; then
        echo "[*] Executando /system/xbin/switch_tv_mode.sh live..."
        /system/xbin/switch_tv_mode.sh live && RESTORATION_OK=1
    elif [ -x "/system/xbin/start_tv.sh" ]; then
        echo "[*] Executando /system/xbin/start_tv.sh..."
        /system/xbin/start_tv.sh && RESTORATION_OK=1
    elif [ -f "$LOCAL_DIR/start_clean.sh" ]; then
        echo "[*] Executando sh $LOCAL_DIR/start_clean.sh..."
        sh "$LOCAL_DIR/start_clean.sh" && RESTORATION_OK=1
    fi

    if [ "$RESTORATION_OK" -eq 1 ]; then
        echo "[✓] TV Ao Vivo restaurada com sucesso como fallback!"
        release_lock
        exit 0
    else
        echo "[!] ALERTA: Falha ao invocar scripts de restauracao da Live TV."
        return 1
    fi
}

echo "[*] ==================================================="
echo "[*]       ROLLBACK INSTANTANEO DO VOD (REVERT V2)      "
echo "[*] ==================================================="

# Adquirir lock atomico via diretorio
acquire_lock

# 1. Verificar integridade rigorosa do backup original (Finding 3)
echo "[*] 1/6: Verificando integridade rigorosa do backup original ($BAK_BIN)..."
BAK_READY=0
if [ -f "$BAK_BIN" ]; then
    if is_valid_elf "$BAK_BIN"; then
        BAK_READY=1
        _sz=$(bb_filesize "$BAK_BIN")
        echo "[✓] Backup original $BAK_BIN validado com sucesso (${_sz} bytes, ELF integro)."
    else
        echo "[!] AVISO: Backup $BAK_BIN corrompido ou truncado (<=100KB ou nao e ELF)!"
    fi
fi

if [ "$BAK_READY" -eq 0 ]; then
    if [ -f "$ORIG_BIN" ] && is_valid_elf "$ORIG_BIN"; then
        echo "[*] $BAK_BIN invalido/ausente, mas $ORIG_BIN e valido. Utilizando $ORIG_BIN."
        BAK_BIN="$ORIG_BIN"
        BAK_READY=1
    fi
fi

if [ "$BAK_READY" -eq 0 ]; then
    echo "[!] AVISO: Nenhum binario integro (nem $BAK_BIN nem $ORIG_BIN) disponivel para restauracao VOD."
    echo "[*] Iniciando fallback para o Modo Live TV..."
    restore_live_mode || {
        echo "[!] ERRO FATAL: Impossivel continuar sem backup ou Modo Live TV!"
        exit 1
    }
fi

# 2. Capturar Task ID ativo ou acionar fallback para Live Mode
echo "[*] 2/6: Identificando Task ID ativo..."
if [ -z "$TASK_ID" ] && [ -f "$FLAG_FILE" ]; then
    TASK_ID=$(cat "$FLAG_FILE" 2>/dev/null | tr -d '\r\n[:space:]')
fi

if [ -z "$TASK_ID" ]; then
    echo "[!] AVISO: Task ID nao informado e nao encontrado em $FLAG_FILE."
    echo "[*] Buscando historico recente em $LOG_FILE..."
    TASK_ID=$(grep -o "/vod/[a-zA-Z0-9_-]*/movie.mp4" "$LOG_FILE" 2>/dev/null | tail -n 1 | cut -d'/' -f3 || true)
fi

if [ -z "$TASK_ID" ]; then
    echo "[!] AVISO: Nenhum Task ID VOD valido identificado."
    echo "[*] Ativando fallback seguro para o Modo Live TV..."
    restore_live_mode || {
        echo "[!] ERRO FATAL: Nao foi possivel determinar Task ID e o fallback para Live falhou!"
        exit 1
    }
fi
echo "[✓] Task ID ativo para rollback: $TASK_ID"

# 3. Interromper V2 e processos FUSE
echo "[*] 3/6: Desconectando LUN USB e finalizando processos FUSE v2..."
if [ -d "/sys/class/android_usb/android0" ]; then
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true
    echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
elif [ -d "/config/usb_gadget/g1" ] || [ -d "/sys/kernel/config/usb_gadget/g1" ]; then
    GADGET="/config/usb_gadget/g1"
    [ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
fi

# Matar v2 e qualquer instancia do fuse_direct
bb_kill_pattern "fuse_direct" 15
sleep 1
bb_kill_pattern "fuse_direct" 9

# Desmontagem FUSE
_u_count=0
while bb_mount_grep "$MNT_POINT"; do
    echo "[*] Desmontando $MNT_POINT..."
    bb_umount "$MNT_POINT"
    sleep 0.5
    _u_count=$((_u_count + 1))
    if [ "$_u_count" -ge 10 ]; then
        echo "[!] Aviso: Atingido limite de 10 tentativas de umount..."
        break
    fi
done

# 4. Restaurar binário original e validar integridade ELF
echo "[*] 4/6: Restaurando $BAK_BIN para $ORIG_BIN..."
if [ "$BAK_BIN" != "$ORIG_BIN" ]; then
    cp -p "$BAK_BIN" "$ORIG_BIN.tmp" 2>/dev/null || cp "$BAK_BIN" "$ORIG_BIN.tmp"
    bb_chmod 755 "$ORIG_BIN.tmp"
    sync 2>/dev/null || true
    mv -f "$ORIG_BIN.tmp" "$ORIG_BIN"
fi
bb_chmod 755 "$ORIG_BIN"

if ! is_valid_elf "$ORIG_BIN"; then
    echo "[!] ERRO FATAL: $ORIG_BIN restaurado nao e um ELF integro!"
    echo "[*] Tentando fallback para TV Ao Vivo..."
    restore_live_mode || exit 1
fi

BIN_SIZE=$(bb_filesize "$ORIG_BIN")
echo "[✓] Binario original restaurado e validado (${BIN_SIZE} bytes)."

# 5. Reiniciar FUSE com binário original
echo "[*] 5/6: Reiniciando FUSE original..."
mkdir -p "$MNT_POINT"
chmod 666 /dev/fuse 2>/dev/null || true
VOD_STREAM_URL="http://$SERVER_HOST/vod/$TASK_ID/movie.mp4"

echo "" >> "$LOG_FILE"
echo "=== [$(date '+%Y-%m-%d %H:%M:%S')] ROLLBACK TO ORIGINAL VOD ($ORIG_BIN) TASK=$TASK_ID ===" >> "$LOG_FILE"

bb_setsid "$ORIG_BIN" "$MNT_POINT" "$VOD_STREAM_URL" "$TMPL_LOCAL" >> "$LOG_FILE" 2>&1 &

# Aguardar montagem do disco virtual
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$BACKING_IMG" ]; then
        break
    fi
    sleep 0.5
done

if [ ! -f "$BACKING_IMG" ]; then
    echo "[!] ERRO: Arquivo backing ($BACKING_IMG) nao foi restaurado em $MNT_POINT!"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    echo "[*] Tentando fallback de seguranca para TV Ao Vivo..."
    restore_live_mode || exit 1
fi

# 6. Reset no USB Gadget e verificacao de conectividade com a TV
echo "[*] 6/6: Resetando USB Gadget e verificando TV..."
if [ -d "/sys/class/android_usb/android0" ]; then
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true
    echo "CINEMA" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
    echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    sleep 1
    echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
    echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true
elif [ -d "/config/usb_gadget/g1" ] || [ -d "/sys/kernel/config/usb_gadget/g1" ]; then
    GADGET="/config/usb_gadget/g1"
    [ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
    UDC=$(getprop sys.usb.controller 2>/dev/null || true)
    [ -z "$UDC" ] && UDC="a800000.dwc3"
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    sleep 1
    echo 1 > "$GADGET/functions/mass_storage.0/lun.0/ro" 2>/dev/null || true
    echo "$BACKING_IMG" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
    rm -f "$GADGET/configs/b.1/f1" "$GADGET/configs/b.1/f2" 2>/dev/null || true
    ln -s "$GADGET/functions/mass_storage.0" "$GADGET/configs/b.1/f1" 2>/dev/null || true
    [ -d "$GADGET/functions/ffs.adb" ] && ln -s "$GADGET/functions/ffs.adb" "$GADGET/configs/b.1/f2" 2>/dev/null || true
    echo "$UDC" > "$GADGET/UDC" 2>/dev/null || true
fi

# Validar processo e conectividade USB
ORIG_PID=$(bb_pgrep "$ORIG_BIN" | tail -n 1)
if [ -n "$ORIG_PID" ] && kill -0 "$ORIG_PID" 2>/dev/null; then
    echo "[✓] Processo original restaurado com sucesso (PID: $ORIG_PID)."
else
    echo "[!] ERRO: Processo FUSE original nao esta ativo!"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    exit 1
fi

# Validar LUN
if [ -d "/sys/class/android_usb/android0" ]; then
    CURRENT_LUN=$(cat /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null)
    USB_ENABLED=$(cat /sys/class/android_usb/android0/enable 2>/dev/null)
    echo "[*] Estado do Gadget USB: LUN=$CURRENT_LUN, Enable=$USB_ENABLED"
    if [ "$CURRENT_LUN" = "$BACKING_IMG" ] && [ "$USB_ENABLED" = "1" ]; then
        echo "[✓] TV conectada com sucesso ao disco virtual original."
    else
        echo "[!] ALERTA: Conexao USB pode nao estar ativa!"
    fi
fi

echo "[*] Ultimas linhas de $LOG_FILE:"
tail -n 15 "$LOG_FILE" 2>/dev/null || true
echo "[✓] Rollback concluido com sucesso!"
