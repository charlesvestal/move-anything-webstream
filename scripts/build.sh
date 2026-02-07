#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-yt-builder"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
  echo "=== YT Module Build (via Docker) ==="
  if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
  fi
  docker run --rm \
    -v "$REPO_ROOT:/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    "$IMAGE_NAME" \
    ./scripts/build.sh
  exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"
rm -rf build/module dist/yt
mkdir -p build/module dist/yt

echo "Compiling v2 DSP plugin..."
"${CROSS_PREFIX}gcc" -O3 -g -shared -fPIC \
  src/dsp/yt_stream_plugin.c \
  -o build/module/dsp.so \
  -Isrc/dsp \
  -lpthread -lm

cat src/module.json > dist/yt/module.json
cat src/ui.js > dist/yt/ui.js
cat build/module/dsp.so > dist/yt/dsp.so
chmod +x dist/yt/dsp.so

if [ -d build/deps/bin ]; then
  echo "Bundling yt-dlp dependencies..."
  mkdir -p dist/yt/bin
  cp build/deps/bin/yt-dlp dist/yt/bin/yt-dlp
  cp build/deps/bin/deno dist/yt/bin/deno
  cp build/deps/bin/ffmpeg dist/yt/bin/ffmpeg
  cp build/deps/bin/ffprobe dist/yt/bin/ffprobe
  chmod +x dist/yt/bin/*
else
  echo "Warning: build/deps/bin not found (run ./scripts/build-deps.sh to bundle yt-dlp deps)"
fi

(
  cd dist
  tar -czvf yt-module.tar.gz yt/
)

echo "=== Build Complete ==="
echo "Module dir: dist/yt"
echo "Tarball: dist/yt-module.tar.gz"
