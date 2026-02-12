#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_JS="$ROOT_DIR/src/ui.js"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

if ! rg -q "host_module_set_param\\('search_query', ''\\)" "$UI_JS"; then
  echo "FAIL: ui.js should clear search_query when entering a fresh search flow"
  fail=1
fi

if ! rg -q 'search_query"\) == 0' "$DSP_C"; then
  echo "FAIL: DSP should handle search_query param"
  fail=1
fi

if ! rg -q "set_search_status\\(inst, \"idle\", \"\"\\)" "$DSP_C"; then
  echo "FAIL: DSP should reset search status to idle when clearing search_query"
  fail=1
fi

if ! rg -q "inst->search_count = 0;" "$DSP_C"; then
  echo "FAIL: DSP should clear prior search results on reset"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: search reset behavior is wired"
