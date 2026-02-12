#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q "infer_provider_from_url\\(" "$DSP_C"; then
  echo "FAIL: DSP should infer provider from stream URL"
  fail=1
fi

if ! rg -q "infer_provider_from_url\\(clean_url, clean_provider" "$DSP_C"; then
  echo "FAIL: stream_url set path should infer provider before routing"
  fail=1
fi

if ! rg -q "soundcloud.com" "$DSP_C"; then
  echo "FAIL: provider inference should recognize soundcloud URLs"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: provider inference wiring is present"
