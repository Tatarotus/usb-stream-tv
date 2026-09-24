#!/bin/bash
# connect_phone.sh - Connect to Mi A2 over Oracle Cloud Reverse Tunnel
# Works anywhere, even when phone is plugged into the TV!

# 1. Ensure SSH tunnel to Oracle port 25556 is active
if ! pgrep -f "25556:127.0.0.1:25556" >/dev/null 2>&1; then
    echo "[*] Opening secure SSH tunnel to Oracle VPS (port 25556)..."
    ssh -f -N -L 25556:127.0.0.1:25556 oracle
fi

# 2. Connect ADB
echo "[*] Connecting ADB to Mi A2..."
adb connect 127.0.0.1:25556

# 3. Open Root Shell or run command
if [ -n "$1" ]; then
    adb -s 127.0.0.1:25556 shell "su -c '$*'"
else
    echo "[✓] Connected! Entering root shell on Mi A2:"
    adb -s 127.0.0.1:25556 shell "su"
fi
