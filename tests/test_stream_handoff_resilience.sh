#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q "supports_legacy_fallback\\(" "$DSP_C"; then
  echo "FAIL: DSP should gate resolved-stream legacy fallback by provider"
  fail=1
fi

if ! rg -q "schedule_stream_reap\\(" "$DSP_C"; then
  echo "FAIL: DSP should asynchronously reap old stream processes"
  fail=1
fi

if ! rg -q "pthread_detach\\(" "$DSP_C"; then
  echo "FAIL: stream reap should run detached to avoid UI blocking"
  fail=1
fi

if ! rg -q "inst->stream_pid" "$DSP_C"; then
  echo "FAIL: DSP should track stream process pid for controlled shutdown"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: stream handoff resilience wiring is present"
