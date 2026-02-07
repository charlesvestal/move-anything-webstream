#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-yt-builder"
BUNDLE_RUNTIME="${BUNDLE_RUNTIME:-auto}"   # auto | with-deps | core-only
OUTPUT_BASENAME="${OUTPUT_BASENAME:-yt-module}"

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
  echo "=== YT Module Build (via Docker) ==="
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
cat src/ui_chain.js > dist/yt/ui_chain.js
cat build/module/dsp.so > dist/yt/dsp.so
chmod +x dist/yt/dsp.so

printf '%s\n' "$BUNDLE_RUNTIME" > dist/yt/runtime_profile.txt

if [ "$bundle_deps" -eq 1 ]; then
  echo "Bundling runtime dependencies..."
  mkdir -p dist/yt/bin
  cp build/deps/bin/yt-dlp dist/yt/bin/yt-dlp
  cp build/deps/bin/deno dist/yt/bin/deno
  cp build/deps/bin/ffmpeg dist/yt/bin/ffmpeg
  cp build/deps/bin/ffprobe dist/yt/bin/ffprobe
  chmod +x dist/yt/bin/*
else
  echo "Building core-only artifact (runtime binaries are user-supplied)"
fi

if [ -f THIRD_PARTY_NOTICES.md ]; then
  cp THIRD_PARTY_NOTICES.md dist/yt/THIRD_PARTY_NOTICES.md
fi
if [ -d licenses ]; then
  rm -rf dist/yt/licenses
  cp -R licenses dist/yt/licenses
fi
if [ -f build/deps/manifest.json ]; then
  cp build/deps/manifest.json dist/yt/THIRD_PARTY_MANIFEST.json
fi

(
  cd dist
  tar -czvf "${OUTPUT_BASENAME}.tar.gz" yt/
)

echo "=== Build Complete ==="
echo "Module dir: dist/yt"
echo "Tarball: dist/${OUTPUT_BASENAME}.tar.gz"
