#!/system/bin/sh
# apply_vod_v2.sh — Zero-Downtime Hot Upgrade to VOD v2
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
SERVER_HOST="${1:-tv.smre.run.place}"

LOCK_OWNED=0
SUCCESS=0
NEEDS_ROLLBACK=0

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

bb_download() {
    _url="$1"
    _dst="$2"
    if [ -n "$BB" ]; then
        "$BB" wget -q -O "$_dst" "$_url" 2>/dev/null && [ -s "$_dst" ] && return 0
    fi
    if command -v wget >/dev/null 2>&1; then
        wget -q -O "$_dst" "$_url" 2>/dev/null && [ -s "$_dst" ] && return 0
    fi
    if [ -x "$LOCAL_DIR/curl" ]; then
        "$LOCAL_DIR/curl" -s -o "$_dst" "$_url" 2>/dev/null && [ -s "$_dst" ] && return 0
    elif command -v curl >/dev/null 2>&1; then
        curl -s -o "$_dst" "$_url" 2>/dev/null && [ -s "$_dst" ] && return 0
    fi
    return 1
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

        # Se o processo dono do lock ja morreu, lock eh orfao (stale)
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

rollback_on_failure() {
    trap '' EXIT INT TERM HUP
    echo ""
    echo "[!] ==================================================="
    echo "[!] FALHA DETECTADA DURANTE A ATUALIZACAO PARA VOD V2! "
    echo "[!] DISPARANDO ROLLBACK AUTOMATICO IMEDIATO...         "
    echo "[!] ==================================================="

    ROLLBACK_SCRIPT="$LOCAL_DIR/rollback_vod.sh"
    if [ ! -f "$ROLLBACK_SCRIPT" ]; then
        SCRIPT_DIR=$(dirname "$0" 2>/dev/null || echo "$LOCAL_DIR")
        [ -f "$SCRIPT_DIR/rollback_vod.sh" ] && ROLLBACK_SCRIPT="$SCRIPT_DIR/rollback_vod.sh"
    fi

    # Liberar lock antes de chamar o rollback para evitar deadlocks
    release_lock

    if [ -f "$ROLLBACK_SCRIPT" ]; then
        echo "[*] Executando rollback: sh $ROLLBACK_SCRIPT ${TASK_ID:-} $SERVER_HOST"
        sh "$ROLLBACK_SCRIPT" ${TASK_ID:+"$TASK_ID"} "$SERVER_HOST" || true
    else
        echo "[!] ERRO CRITICO: Script de rollback ($ROLLBACK_SCRIPT) nao encontrado!"
    fi
    exit 1
}

cleanup_and_exit() {
    _code=$?
    release_lock
    if [ "$SUCCESS" -ne 1 ] && [ "$NEEDS_ROLLBACK" -eq 1 ]; then
        rollback_on_failure
    fi
    exit "$_code"
}

trap 'cleanup_and_exit' EXIT INT TERM HUP

echo "[*] ==================================================="
echo "[*]        UPGRADE VOD -> V2 (ZERO-DOWNTIME STAGING)    "
echo "[*] ==================================================="

# Adquirir lock atomico via diretorio
acquire_lock

# 1. Pre-flight Sanity Check e Backup ANTES de parar qualquer processo
echo "[*] 1/5: Validando binarios e garantindo backup original integro..."

# Finding 3: Validar integridade de backup existente ou criar atomicamente a partir de $ORIG_BIN
BAK_VALID=0
if [ -f "$BAK_BIN" ]; then
    if is_valid_elf "$BAK_BIN"; then
        BAK_VALID=1
        echo "[*] Backup original existente e integro validado em $BAK_BIN."
    else
        echo "[!] AVISO: Backup existente em $BAK_BIN esta corrompido/truncado (<=100KB ou nao e ELF)!"
        rm -f "$BAK_BIN" 2>/dev/null || true
    fi
fi

if [ "$BAK_VALID" -eq 0 ]; then
    if [ -f "$ORIG_BIN" ]; then
        if ! is_valid_elf "$ORIG_BIN"; then
            echo "[!] ERRO FATAL: $ORIG_BIN existe mas nao e um binario ELF valido para backup!"
            exit 1
        fi
        echo "[*] Criando backup atomico: $ORIG_BIN -> $BAK_BIN.tmp -> $BAK_BIN..."
        rm -f "$BAK_BIN.tmp" 2>/dev/null || true
        cp -p "$ORIG_BIN" "$BAK_BIN.tmp" 2>/dev/null || cp "$ORIG_BIN" "$BAK_BIN.tmp"
        bb_chmod 755 "$BAK_BIN.tmp"
        sync 2>/dev/null || true
        mv -f "$BAK_BIN.tmp" "$BAK_BIN"
        if is_valid_elf "$BAK_BIN"; then
            echo "[✓] Backup criado atomicamente e validado com sucesso ($BAK_BIN)."
        else
            echo "[!] ERRO FATAL: Falha ao gerar backup integro em $BAK_BIN!"
            exit 1
        fi
    else
        echo "[!] ERRO FATAL: Nem $BAK_BIN valido nem $ORIG_BIN foram encontrados!"
        exit 1
    fi
fi

# Validar existencia, permissoes e integridade do novo binario V2
if [ ! -f "$V2_BIN" ]; then
    echo "[!] ERRO FATAL: Binario $V2_BIN nao encontrado!"
    exit 1
fi

bb_chmod 755 "$V2_BIN"
if [ ! -x "$V2_BIN" ]; then
    echo "[!] ERRO FATAL: $V2_BIN nao possui permissao de execucao!"
    exit 1
fi

BIN_SIZE=$(bb_filesize "$V2_BIN")
echo "[*] Tamanho do binario V2: ${BIN_SIZE} bytes"
if [ "$BIN_SIZE" -le 102400 ]; then
    echo "[!] ERRO FATAL: $V2_BIN tem tamanho insuficiente (${BIN_SIZE} bytes <= 100KB)!"
    exit 1
fi

if ! is_valid_elf "$V2_BIN"; then
    echo "[!] ERRO FATAL: $V2_BIN nao e um binario ELF valido (cabecalho corrompido)!"
    exit 1
fi
echo "[✓] Sanity check do binario $V2_BIN concluido com sucesso."

# 2. Capturar Task ID ativo de vod_mode.flag
echo "[*] 2/5: Obtendo Task ID ativo do VOD..."
if [ ! -f "$FLAG_FILE" ]; then
    echo "[!] ERRO FATAL: Flag file $FLAG_FILE nao encontrado! VOD nao esta em execucao."
    exit 1
fi

TASK_ID=$(cat "$FLAG_FILE" 2>/dev/null | tr -d '\r\n[:space:]')
if [ -z "$TASK_ID" ]; then
    echo "[!] ERRO FATAL: Task ID vazio em $FLAG_FILE!"
    exit 1
fi
echo "[✓] Task ID ativo identificado: $TASK_ID"

# Garantir template FAT32
if [ ! -s "$TMPL_LOCAL" ]; then
    echo "[*] Template FAT32 local nao encontrado. Baixando de $SERVER_HOST..."
    TMPL_URL="http://$SERVER_HOST/vod/$TASK_ID/template.bin"
    bb_download "$TMPL_URL" "$TMPL_LOCAL" || true
fi

if [ ! -s "$TMPL_LOCAL" ]; then
    echo "[!] ERRO FATAL: Template FAT32 ($TMPL_LOCAL) vazio ou inexistente!"
    exit 1
fi

# 3. Desmontar FUSE graciosamente e finalizar processo existente
# A partir daqui o servico atual sera interrompido; falhas exigem rollback
NEEDS_ROLLBACK=1
echo "[*] 3/5: Desconectando USB e parando processo FUSE anterior..."

# Desconectar LUN para liberar backing file do kernel
if [ -d "/sys/class/android_usb/android0" ]; then
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true
    echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
elif [ -d "/config/usb_gadget/g1" ] || [ -d "/sys/kernel/config/usb_gadget/g1" ]; then
    GADGET="/config/usb_gadget/g1"
    [ ! -d "$GADGET" ] && GADGET="/sys/kernel/config/usb_gadget/g1"
    echo "" > "$GADGET/UDC" 2>/dev/null || true
    echo "" > "$GADGET/functions/mass_storage.0/lun.0/file" 2>/dev/null || true
fi

# Sinalizar encerramento do FUSE anterior
bb_kill_pattern "fuse_direct" 15
sleep 1
bb_kill_pattern "fuse_direct" 9

# Desmontagem FUSE
_u_count=0
while bb_mount_grep "$MNT_POINT"; do
    echo "[*] Executando umount -l $MNT_POINT..."
    bb_umount "$MNT_POINT"
    sleep 0.5
    _u_count=$((_u_count + 1))
    if [ "$_u_count" -ge 10 ]; then
        echo "[!] Aviso: Atingido limite de 10 tentativas de umount, forcando prosseguimento..."
        break
    fi
done

# 4. Iniciar novo processo FUSE com fuse_direct_arm32_v2
echo "[*] 4/5: Iniciando $V2_BIN..."
mkdir -p "$MNT_POINT"
chmod 666 /dev/fuse 2>/dev/null || true
VOD_STREAM_URL="http://$SERVER_HOST/vod/$TASK_ID/movie.mp4"

echo "" >> "$LOG_FILE"
echo "=== [$(date '+%Y-%m-%d %H:%M:%S')] STARTING VOD V2 ($V2_BIN) TASK=$TASK_ID ===" >> "$LOG_FILE"

bb_setsid "$V2_BIN" "$MNT_POINT" "$VOD_STREAM_URL" "$TMPL_LOCAL" >> "$LOG_FILE" 2>&1 &

# Aguardar inicializacao e presenca do arquivo virtual
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$BACKING_IMG" ]; then
        break
    fi
    sleep 0.5
done

if [ ! -f "$BACKING_IMG" ]; then
    echo "[!] ERRO FATAL: Arquivo backing ($BACKING_IMG) nao apareceu em $MNT_POINT!"
    echo "[!] Log do FUSE:"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    rollback_on_failure
fi

# 5. Soft-reset no USB Gadget para a TV reconectar sem travamentos
echo "[*] 5/5: Executando soft-reset do USB Gadget..."
if [ -d "/sys/class/android_usb/android0" ]; then
    # SM-T110 sysfs legado
    echo 0 > /sys/class/android_usb/android0/enable 2>/dev/null || true
    echo "CINEMA" > /sys/class/android_usb/android0/f_mass_storage/inquiry_string 2>/dev/null || true
    echo "" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    sleep 1
    echo 1 > /sys/class/android_usb/android0/f_mass_storage/lun0/ro 2>/dev/null || true
    echo "$BACKING_IMG" > /sys/class/android_usb/android0/f_mass_storage/lun0/file 2>/dev/null || true
    echo 1 > /sys/class/android_usb/android0/enable 2>/dev/null || true
elif [ -d "/config/usb_gadget/g1" ] || [ -d "/sys/kernel/config/usb_gadget/g1" ]; then
    # ConfigFS moderno
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

# Verificação final de processo e log
NEW_PID=$(bb_pgrep "$V2_BIN" | tail -n 1)
if [ -n "$NEW_PID" ] && kill -0 "$NEW_PID" 2>/dev/null; then
    echo "[✓] VOD V2 aplicado com sucesso! PID: $NEW_PID"
else
    echo "[!] ALERTA: Processo VOD V2 nao detectado!"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    rollback_on_failure
fi

echo "[*] Status das ultimas linhas de $LOG_FILE:"
tail -n 15 "$LOG_FILE" 2>/dev/null || true

SUCCESS=1
NEEDS_ROLLBACK=0
echo "[✓] Upgrade VOD V2 concluido com sucesso."
