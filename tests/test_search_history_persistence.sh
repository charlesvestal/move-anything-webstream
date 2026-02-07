#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_JS="$ROOT_DIR/src/ui.js"

fail=0

if ! rg -q "from 'std'" "$UI_JS"; then
  echo "FAIL: ui.js should import std for history persistence"
  fail=1
fi

if ! rg -q "from 'os'" "$UI_JS"; then
  echo "FAIL: ui.js should import os for atomic history writes"
  fail=1
fi

if ! rg -q "const SEARCH_HISTORY_PATH = '/data/UserData/move-anything/yt_search_history.json'" "$UI_JS"; then
  echo "FAIL: ui.js should define shared on-disk history path"
  fail=1
fi

if ! rg -q "saveSearchHistoryToDisk\\(" "$UI_JS"; then
  echo "FAIL: ui.js should implement saveSearchHistoryToDisk()"
  fail=1
fi

if ! rg -q "loadSearchHistoryFromDisk\\(" "$UI_JS"; then
  echo "FAIL: ui.js should implement loadSearchHistoryFromDisk()"
  fail=1
fi

if ! awk '
  /function submitSearch\(query\)/ { in_fn=1; depth=0; next }
  in_fn {
    if (index($0, "{") > 0) depth++
    if ($0 ~ /saveSearchHistoryToDisk\(\)/) found=1
    if (index($0, "}") > 0 && depth == 0) { in_fn=0 }
    if (index($0, "}") > 0 && depth > 0) depth--
  }
  END { exit(found ? 0 : 1) }
' "$UI_JS"; then
  echo "FAIL: submitSearch() should persist history after updating it"
  fail=1
fi

if ! awk '
  /function openSearchHistoryMenu\(\)/ { in_fn=1; depth=0; next }
  in_fn {
    if (index($0, "{") > 0) depth++
    if ($0 ~ /loadSearchHistoryFromDisk\(\)/) found=1
    if (index($0, "}") > 0 && depth == 0) { in_fn=0 }
    if (index($0, "}") > 0 && depth > 0) depth--
  }
  END { exit(found ? 0 : 1) }
' "$UI_JS"; then
  echo "FAIL: opening history menu should reload shared history from disk"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: search history persistence wiring is present"
