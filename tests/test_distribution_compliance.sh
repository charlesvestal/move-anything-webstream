#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

fail=0

expect_file() {
  local path="$1"
  if [[ ! -f "$ROOT_DIR/$path" ]]; then
    echo "FAIL: missing file $path"
    fail=1
  fi
}

expect_file "THIRD_PARTY_NOTICES.md"
expect_file "licenses/yt-dlp-UNLICENSE.txt"
expect_file "licenses/deno-MIT.txt"
expect_file "licenses/GPL-3.0.txt"
expect_file "scripts/build-release-assets.sh"

if ! rg -q "BUNDLE_RUNTIME" "$ROOT_DIR/scripts/build.sh"; then
  echo "FAIL: scripts/build.sh should support BUNDLE_RUNTIME profile selection"
  fail=1
fi

if ! rg -q "THIRD_PARTY_NOTICES.md" "$ROOT_DIR/scripts/build.sh"; then
  echo "FAIL: scripts/build.sh should package third-party notices"
  fail=1
fi

if ! rg -q "webstream-module-core.tar.gz" "$ROOT_DIR/.github/workflows/release.yml"; then
  echo "FAIL: release workflow should publish a core-only asset"
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "PASS: distribution compliance wiring is present"
