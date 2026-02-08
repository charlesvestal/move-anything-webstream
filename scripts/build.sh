#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-webstream-builder"
BUNDLE_RUNTIME="${BUNDLE_RUNTIME:-auto}"   # auto | with-deps | core-only
OUTPUT_BASENAME="${OUTPUT_BASENAME:-webstream-module}"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
  echo "=== Webstream Module Build (via Docker) ==="
  if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
    docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
  fi
  docker run --rm \
    -v "$REPO_ROOT:/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    -e BUNDLE_RUNTIME="$BUNDLE_RUNTIME" \
    -e OUTPUT_BASENAME="$OUTPUT_BASENAME" \
    "$IMAGE_NAME" \
    ./scripts/build.sh
  exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

bundle_deps=0
case "$BUNDLE_RUNTIME" in
  auto)
    if [ -d "$REPO_ROOT/build/deps/bin" ]; then
      bundle_deps=1
    fi
    ;;
  with-deps)
    bundle_deps=1
    ;;
  core-only)
    bundle_deps=0
    ;;
  *)
    echo "Invalid BUNDLE_RUNTIME: $BUNDLE_RUNTIME (expected auto|with-deps|core-only)"
    exit 1
    ;;
esac

if [ "$bundle_deps" -eq 1 ] && [ ! -d "$REPO_ROOT/build/deps/bin" ]; then
  echo "Missing build/deps/bin for BUNDLE_RUNTIME=with-deps (run ./scripts/build-deps.sh first)"
  exit 1
fi

cd "$REPO_ROOT"
rm -rf build/module dist/webstream
mkdir -p build/module dist/webstream

echo "Compiling v2 DSP plugin..."
"${CROSS_PREFIX}gcc" -O3 -g -shared -fPIC \
  src/dsp/yt_stream_plugin.c \
  -o build/module/dsp.so \
  -Isrc/dsp \
  -lpthread -lm

cat src/module.json > dist/webstream/module.json
cat src/ui.js > dist/webstream/ui.js
cat src/ui_chain.js > dist/webstream/ui_chain.js
cat build/module/dsp.so > dist/webstream/dsp.so
chmod +x dist/webstream/dsp.so

printf '%s\n' "$BUNDLE_RUNTIME" > dist/webstream/runtime_profile.txt

mkdir -p dist/webstream/bin
cp src/bin/yt_dlp_daemon.py dist/webstream/bin/yt_dlp_daemon.py
chmod +x dist/webstream/bin/yt_dlp_daemon.py

if [ "$bundle_deps" -eq 1 ]; then
  echo "Bundling runtime dependencies..."
  cp build/deps/bin/yt-dlp dist/webstream/bin/yt-dlp
  cp build/deps/bin/deno dist/webstream/bin/deno
  cp build/deps/bin/ffmpeg dist/webstream/bin/ffmpeg
  cp build/deps/bin/ffprobe dist/webstream/bin/ffprobe
  chmod +x dist/webstream/bin/*
else
  echo "Building core-only artifact (runtime binaries are user-supplied)"
fi

if [ -f THIRD_PARTY_NOTICES.md ]; then
  cp THIRD_PARTY_NOTICES.md dist/webstream/THIRD_PARTY_NOTICES.md
fi
if [ -d licenses ]; then
  rm -rf dist/webstream/licenses
  cp -R licenses dist/webstream/licenses
fi
if [ -f build/deps/manifest.json ]; then
  cp build/deps/manifest.json dist/webstream/THIRD_PARTY_MANIFEST.json
fi

(
  cd dist
  tar -czvf "${OUTPUT_BASENAME}.tar.gz" webstream/
)

echo "=== Build Complete ==="
echo "Module dir: dist/webstream"
echo "Tarball: dist/${OUTPUT_BASENAME}.tar.gz"
