#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UI_JS="$ROOT_DIR/src/ui.js"

fail=0

if ! rg -q "const PROVIDER_TAGS" "$UI_JS"; then
  echo "FAIL: ui.js should define short provider tags ([YT]/[SC]/[AR]/[FS])"
  fail=1
fi

if ! rg -Fq 'title: `Webstream ${providerTag(searchProvider)}`' "$UI_JS"; then
  echo "FAIL: root menu title should show active source tag"
  fail=1
fi

if ! rg -q "initialText: ''" "$UI_JS"; then
  echo "FAIL: new search prompt should start with empty text"
  fail=1
fi

if ! rg -q "host_module_set_param\\('search_query', ''\\)" "$UI_JS"; then
  echo "FAIL: opening a new search should clear existing search state"
  fail=1
fi

if rg -q "\\$\\{providerLabel\\(rowProvider\\)\\}: \\$\\{row\\?\\.title" "$UI_JS"; then
  echo "FAIL: result rows should not prefix long provider labels"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: UI search flow and provider tag behavior are wired"
