#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_PY="$ROOT_DIR/src/bin/yt_dlp_daemon.py"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

if [[ ! -f "$DAEMON_PY" || ! -f "$DSP_C" ]]; then
  echo "FAIL: expected source files missing"
  exit 1
fi

if ! rg -q 'opts\["format"\] = "http_mp3_1_0/hls_mp3_1_0/bestaudio"' "$DAEMON_PY"; then
  echo "FAIL: soundcloud resolve should prefer full MP3 formats before generic bestaudio"
  exit 1
fi

if ! rg -q 'legacy_fmt = "http_mp3_1_0/hls_mp3_1_0/bestaudio"' "$DSP_C"; then
  echo "FAIL: legacy soundcloud stream path should prefer full MP3 formats before generic bestaudio"
  exit 1
fi

echo "PASS: soundcloud quality preference wiring is present"
