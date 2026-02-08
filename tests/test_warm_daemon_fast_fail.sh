#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DSP_C="$ROOT_DIR/src/dsp/yt_stream_plugin.c"

fail=0

# Search/resolve request paths should fail fast via daemon.
# Slow legacy fallbacks in these control paths cause long UI stalls/unload hangs.
if rg -q "return run_search_command_legacy\\(" "$DSP_C"; then
  echo "FAIL: run_search_command() should not fall back to legacy search path"
  fail=1
fi

if rg -q "return resolve_stream_url_legacy\\(" "$DSP_C"; then
  echo "FAIL: resolve_stream_url() should not fall back to legacy resolve path"
  fail=1
fi

if ! rg -q "DAEMON_SEARCH_TIMEOUT_MS" "$DSP_C"; then
  echo "FAIL: daemon search timeout constant missing"
  fail=1
fi

if ! rg -q "DAEMON_RESOLVE_TIMEOUT_MS" "$DSP_C"; then
  echo "FAIL: daemon resolve timeout constant missing"
  fail=1
fi

stream_url_max="$(rg -o '#define STREAM_URL_MAX[[:space:]]+[0-9]+' "$DSP_C" | awk '{print $3}' | head -n1)"
if [[ -z "${stream_url_max:-}" || "$stream_url_max" -lt 2048 ]]; then
  echo "FAIL: STREAM_URL_MAX should be at least 2048 for resolved media URLs (got '${stream_url_max:-<missing>}')"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: daemon control paths are fail-fast"
