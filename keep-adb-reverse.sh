#!/usr/bin/env bash
# keep-adb-reverse.sh — keeps adb reverse tcp:8080 alive while phone is attached.
# The writer depends on 127.0.0.1:8080 over USB; every adb daemon restart
# silently drops the forward and the writer stalls with ECONNREFUSED.
while true; do
    if adb devices 2>/dev/null | grep -qE "device$"; then
        if ! adb reverse --list 2>/dev/null | grep -q "tcp:8080"; then
            adb reverse tcp:8080 tcp:8080 >/dev/null 2>&1 || true
        fi
    fi
    sleep 20
done
