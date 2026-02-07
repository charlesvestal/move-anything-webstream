#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_JSON="$ROOT_DIR/src/module.json"
RELEASE_JSON="$ROOT_DIR/release.json"
REPO="charlesvestal/move-anything-yt"

module_version="$(jq -r '.version' "$MODULE_JSON")"
release_version="$(jq -r '.version' "$RELEASE_JSON")"
download_url="$(jq -r '.download_url' "$RELEASE_JSON")"
download_url_core="$(jq -r '.download_url_core // empty' "$RELEASE_JSON")"
expected_url="https://github.com/${REPO}/releases/download/v${release_version}/yt-module.tar.gz"
expected_url_core="https://github.com/${REPO}/releases/download/v${release_version}/yt-module-core.tar.gz"

if [[ "$release_version" != "$module_version" ]]; then
  echo "FAIL: release.json version ($release_version) != module version ($module_version)"
  exit 1
fi

if [[ "$download_url" != "$expected_url" ]]; then
  echo "FAIL: release.json download_url mismatch"
  echo "  expected: $expected_url"
  echo "  actual:   $download_url"
  exit 1
fi

if [[ -n "$download_url_core" && "$download_url_core" != "$expected_url_core" ]]; then
  echo "FAIL: release.json download_url_core mismatch"
  echo "  expected: $expected_url_core"
  echo "  actual:   $download_url_core"
  exit 1
fi

echo "PASS: release metadata matches module version and expected release asset URL"
