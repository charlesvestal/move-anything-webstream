#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q "append_ws_log\\(" "$DSP_C"; then
  echo "FAIL: DSP should expose append_ws_log() for device diagnostics"
  fail=1
fi

if ! rg -q "webstream-runtime.log" "$DSP_C"; then
  echo "FAIL: runtime log path should be defined for ws diagnostics"
  fail=1
fi

if ! rg -q "append_ws_log\\(msg\\)" "$DSP_C"; then
  echo "FAIL: yt_log should mirror messages into runtime log file"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: runtime logging wiring is present"
