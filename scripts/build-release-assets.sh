#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d build/deps/bin ]; then
  echo "Missing build/deps/bin. Run ./scripts/build-deps.sh first."
  exit 1
fi

echo "=== Building release asset: with bundled runtime ==="
BUNDLE_RUNTIME=with-deps OUTPUT_BASENAME=webstream-module ./scripts/build.sh

echo "=== Building release asset: core-only (user-supplied runtime) ==="
BUNDLE_RUNTIME=core-only OUTPUT_BASENAME=webstream-module-core ./scripts/build.sh

echo "=== Release assets ready ==="
ls -lh dist/webstream-module*.tar.gz
