#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q 'strcmp\(provider, "soundcloud"\)' "$DSP_C"; then
  echo "FAIL: DSP should have explicit soundcloud handling in stream startup"
  fail=1
fi

if ! rg -q -- "youtube:player_skip=js" "$DSP_C"; then
  echo "FAIL: DSP should still support youtube extractor args for youtube legacy path"
  fail=1
fi

if ! rg -q -- "legacy_fmt = \"http_mp3_1_0/hls_mp3_1_0/bestaudio\"" "$DSP_C"; then
  echo "FAIL: DSP should prefer full MP3 formats for soundcloud legacy path"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: soundcloud stream path wiring is present"
