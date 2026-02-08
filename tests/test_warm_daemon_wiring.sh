#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_SH="$ROOT_DIR/scripts/build.sh"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"
DAEMON_SRC="$ROOT_DIR/src/bin/yt_dlp_daemon.py"

fail=0

if [[ ! -f "$DAEMON_SRC" ]]; then
  echo "FAIL: missing daemon helper script at src/bin/yt_dlp_daemon.py"
  fail=1
fi

if ! rg -q "yt_dlp_daemon.py" "$BUILD_SH"; then
  echo "FAIL: scripts/build.sh should package yt_dlp_daemon.py into dist/webstream/bin/"
  fail=1
fi

if ! rg -q "yt_dlp_daemon.py" "$DSP_C"; then
  echo "FAIL: DSP should reference the bundled yt_dlp_daemon.py helper"
  fail=1
fi

if ! rg -q "RESOLVE" "$DSP_C"; then
  echo "FAIL: DSP should use daemon resolve requests for warm extraction"
  fail=1
fi

if ! rg -q "SEARCH" "$DSP_C"; then
  echo "FAIL: DSP should use daemon search requests for warm extraction"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: warm daemon wiring is present"
