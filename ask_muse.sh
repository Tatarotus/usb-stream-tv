#!/usr/bin/env bash
# Script to allow Gemini (Antigravity) or developer to consult with Muse Spark (OpenCode) directly from shell.
set -euo pipefail

SESSION_ID="ses_f3f33f976ffe2euaN3RDnWCZoo"

# Fallback: if session ID not found or changed, locate by title
if ! opencode session list --format json 2>/dev/null | grep -q "$SESSION_ID"; then
    FOUND_ID=$(opencode session list --format json 2>/dev/null | python3 -c '
import sys, json
try:
    sessions = json.load(sys.stdin)
    for s in sessions:
        title = s.get("title", "").lower()
        if "handover" in title or "architecture" in title:
            print(s["id"])
            break
except Exception:
    pass
' || true)
    if [ -n "$FOUND_ID" ]; then
        SESSION_ID="$FOUND_ID"
    fi
fi

if [ -z "${*:-}" ]; then
    echo "Usage: ./ask_muse.sh \"Your question or request for Muse Spark\""
    exit 1
fi

TERM=dumb opencode run --session "$SESSION_ID" --auto "$*"
