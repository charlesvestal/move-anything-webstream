#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_JSON="$ROOT_DIR/src/module.json"

missing=0

if jq -e '.capabilities.ui_hierarchy' "$MODULE_JSON" >/dev/null; then
  echo "FAIL: capabilities.ui_hierarchy must be absent (it overrides custom search UI)"
  missing=1
fi

for key in play_pause_step rewind_15_step forward_15_step stop_step restart_step; do
  type_val="$(jq -r --arg key "$key" '.capabilities.chain_params[] | select(.key == $key) | .type // empty' "$MODULE_JSON")"
  if [[ "$type_val" != "enum" ]]; then
    echo "FAIL: $key expected type=enum, got '${type_val:-<missing>}'"
    missing=1
  fi

  options="$(jq -r --arg key "$key" '.capabilities.chain_params[] | select(.key == $key) | (if (.options | type) == "array" then (.options | join(",")) else empty end)' "$MODULE_JSON")"
  if [[ "$options" != "idle,trigger" ]]; then
    echo "FAIL: $key expected options=idle,trigger, got '${options:-<missing>}'"
    missing=1
  fi
done

expected_order="play_pause_step,rewind_15_step,forward_15_step,gain,gain,gain,stop_step,restart_step"
actual_order="$(jq -r '[.capabilities.chain_params[].key] | join(",")' "$MODULE_JSON")"
if [[ "$actual_order" != "$expected_order" ]]; then
  echo "FAIL: chain_params order mismatch"
  echo "  expected: $expected_order"
  echo "  actual:   $actual_order"
  missing=1
fi

if [[ "$missing" -ne 0 ]]; then
  exit 1
fi

echo "PASS: transport params use enum triggers and custom UI-safe metadata"
