#!/bin/bash
# connect_tablet.sh - Connect to tablet over Oracle Cloud Reverse Tunnel
# Works anywhere, even when tablet is plugged into the TV!

# 1. Ensure SSH tunnel to Oracle is active
if ! pgrep -f "25555:127.0.0.1:25555" >/dev/null 2>&1; then
    echo "[*] Opening secure SSH tunnel to Oracle VPS..."
    ssh -f -N -L 25555:127.0.0.1:25555 oracle
fi

# 2. Connect ADB
echo "[*] Connecting ADB to tablet..."
adb connect 127.0.0.1:25555

# 3. Open Root Shell or run command
if [ -n "$1" ]; then
    adb -s 127.0.0.1:25555 shell "su -c '$*'"
else
    echo "[✓] Connected! Entering root shell:"
    adb -s 127.0.0.1:25555 shell "su"
fi
