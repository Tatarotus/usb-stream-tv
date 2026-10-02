#!/usr/bin/env bash
# deploy_tablet.sh — Automação de deploy dos binários e scripts no tablet (SM-T110)
set -euo pipefail

ADB_TARGET="${1:-127.0.0.1:25555}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "============================================================"
echo " Deploying USB-Stream-TV to Tablet (${ADB_TARGET})"
echo "============================================================"

# 1. Verificar conexão ADB
echo "[1/6] Verificando conexão ADB..."
adb connect "$ADB_TARGET" || true
adb -s "$ADB_TARGET" wait-for-device

# 2. Criar diretórios de trabalho no tablet
echo "[2/6] Criando estrutura de diretórios no tablet..."
adb -s "$ADB_TARGET" shell "mkdir -p /data/local/tmp/ntfs_lab/mnt"

# 3. Transferir template NTFS se não existir
echo "[3/6] Verificando template NTFS no tablet..."
if ! adb -s "$ADB_TARGET" shell "[ -f /data/local/tmp/ntfs_lab/ntfs_template.bin ]"; then
    echo "       Enviando e extraindo ntfs_template.tar.gz..."
    adb -s "$ADB_TARGET" push "$ROOT_DIR/templates/ntfs_template.tar.gz" /data/local/tmp/ntfs_lab/
    adb -s "$ADB_TARGET" shell "tar -xzf /data/local/tmp/ntfs_lab/ntfs_template.tar.gz -C /data/local/tmp/ntfs_lab/ && rm -f /data/local/tmp/ntfs_lab/ntfs_template.tar.gz"
else
    echo "       Template NTFS já presente."
fi

# 4. Transferir ferramentas para /data/local/tmp
echo "[4/6] Enviando utilitário patch_trp..."
adb -s "$ADB_TARGET" push "$ROOT_DIR/src/tools/patch_trp_arm32" /data/local/tmp/patch_trp
adb -s "$ADB_TARGET" shell "chmod 755 /data/local/tmp/patch_trp"

# 5. Instalar binários e scripts em /system/xbin (Root)
echo "[5/6] Instalando executáveis e scripts em /system/xbin/..."
adb -s "$ADB_TARGET" push "$ROOT_DIR/src/ntfs/fuse_ntfs_arm32" /data/local/tmp/fuse_ntfs
adb -s "$ADB_TARGET" push "$ROOT_DIR/src/client/stream_fetcher_arm32" /data/local/tmp/stream_fetcher

for s in switch_tv_mode.sh switch_live.sh switch_vod.sh reconnect_usb.sh tv_watchdog.sh; do
    adb -s "$ADB_TARGET" push "$SCRIPT_DIR/$s" "/data/local/tmp/$s"
done

adb -s "$ADB_TARGET" shell "su -c '
    mount -o remount,rw /system &&
    killall -9 fuse_ntfs stream_fetcher 2>/dev/null || true
    sleep 1
    rm -f /system/xbin/fuse_ntfs /system/xbin/stream_fetcher
    cp /data/local/tmp/fuse_ntfs /system/xbin/fuse_ntfs
    cp /data/local/tmp/stream_fetcher /system/xbin/stream_fetcher
    chmod 755 /system/xbin/fuse_ntfs /system/xbin/stream_fetcher
    for f in switch_tv_mode.sh switch_live.sh switch_vod.sh reconnect_usb.sh tv_watchdog.sh; do
        cp /data/local/tmp/\$f /system/xbin/\$f
        chmod 755 /system/xbin/\$f
    done
    cp /system/xbin/switch_live.sh /data/local/tmp/switch_live.sh
    chmod 755 /data/local/tmp/switch_live.sh
    mount -o remount,ro /system
'"

# 6. Reiniciar o serviço de TV Ao Vivo
echo "[6/6] Reiniciando serviço Live TV no tablet..."
adb -s "$ADB_TARGET" shell "su -c '/system/xbin/switch_tv_mode.sh live'"

echo "============================================================"
echo " [✓] Deploy no tablet concluído com sucesso!"
echo "============================================================"
