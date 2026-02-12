#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q "char input_copy\\[PROVIDER_MAX\\]" "$DSP_C"; then
  echo "FAIL: normalize_provider_value should copy input for alias safety"
  fail=1
fi

if ! rg -q "src = input_copy" "$DSP_C"; then
  echo "FAIL: normalize_provider_value should parse from copied input"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: provider normalization handles in-place aliasing safely"
