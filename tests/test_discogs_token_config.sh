#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEMA="$ROOT_DIR/src/settings-schema.json"
DAEMON_PY="$ROOT_DIR/src/bin/yt_dlp_daemon.py"
BUILD_SH="$ROOT_DIR/scripts/build.sh"

fail=0

# Module ships a settings-schema.json declaring the Discogs token as a password
if [[ ! -f "$SCHEMA" ]]; then
  echo "FAIL: src/settings-schema.json should exist"
  fail=1
else
  if ! python3 -c "import json,sys; json.load(open('$SCHEMA'))" 2>/dev/null; then
    echo "FAIL: settings-schema.json should be valid JSON"
    fail=1
  fi
  # id must match the install directory name for schwung-manager discovery
  if ! rg -q '"id"\s*:\s*"webstream"' "$SCHEMA"; then
    echo "FAIL: settings-schema.json id should be \"webstream\""
    fail=1
  fi
  for key in discogs_token freesound_api_key; do
    if ! rg -q "\"key\"\s*:\s*\"${key}\"" "$SCHEMA"; then
      echo "FAIL: settings-schema.json should declare a ${key} field"
      fail=1
    fi
  done
  if ! rg -q '"type"\s*:\s*"password"' "$SCHEMA"; then
    echo "FAIL: secrets should be password fields"
    fail=1
  fi
fi

# Daemon reads the schwung-manager managed secrets
for token in read_module_secret "secrets" 'discogs_token' 'freesound_api_key'; do
  if ! rg -Fq "$token" "$DAEMON_PY"; then
    echo "FAIL: daemon should reference ${token}"
    fail=1
  fi
done

# Schema is packaged into the release tarball
if ! rg -q "settings-schema.json" "$BUILD_SH"; then
  echo "FAIL: build.sh should package settings-schema.json"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: Discogs token config wiring is present across all layers"
