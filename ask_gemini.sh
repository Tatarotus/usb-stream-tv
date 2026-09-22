#!/usr/bin/env bash
# Script to allow Muse (OpenCode) to consult with Gemini (Antigravity) directly from shell.
if [ -z "$*" ]; then
    echo "Usage: ./ask_gemini.sh \"Your question or request for Gemini\""
    exit 1
fi

agy --dangerously-skip-permissions -p "$*"
