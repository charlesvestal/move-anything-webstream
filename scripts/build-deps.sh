#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$REPO_ROOT/build/deps/bin"
WORK_DIR="$REPO_ROOT/build/deps/work"

mkdir -p "$OUT_DIR" "$WORK_DIR"

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1"; exit 1; }
}

require_cmd curl
require_cmd tar
require_cmd unzip
require_cmd git
require_cmd make
require_cmd python3

echo "=== Building yt-dlp (zipimport binary with lazy extractors) ==="
YTDLP_DIR="$WORK_DIR/yt-dlp-src"
if [ ! -d "$YTDLP_DIR/.git" ]; then
  git clone --depth 1 https://github.com/yt-dlp/yt-dlp.git "$YTDLP_DIR"
fi
(
  cd "$YTDLP_DIR"
  git pull --ff-only || true
  make clean >/dev/null 2>&1 || true
  make lazy-extractors yt-dlp
  cp yt-dlp "$OUT_DIR/yt-dlp"
)
chmod +x "$OUT_DIR/yt-dlp"

echo "=== Downloading deno (aarch64 linux) ==="
DENO_VERSION="${DENO_VERSION:-}"
if [ -z "$DENO_VERSION" ]; then
  DENO_VERSION="$(python3 - <<'PY'
import json, urllib.request
with urllib.request.urlopen('https://api.github.com/repos/denoland/deno/releases/latest', timeout=30) as r:
    data = json.load(r)
print(data['tag_name'])
PY
)"
fi
curl -fL -o "$WORK_DIR/deno.zip" "https://github.com/denoland/deno/releases/download/${DENO_VERSION}/deno-aarch64-unknown-linux-gnu.zip"
unzip -o "$WORK_DIR/deno.zip" -d "$WORK_DIR/deno" >/dev/null
cp "$WORK_DIR/deno/deno" "$OUT_DIR/deno"
chmod +x "$OUT_DIR/deno"


echo "=== Downloading ffmpeg (yt-dlp arm64 build) ==="
curl -fL -o "$WORK_DIR/ffmpeg.tar.xz" "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz"
rm -rf "$WORK_DIR/ffmpeg-extract"
mkdir -p "$WORK_DIR/ffmpeg-extract"
tar -xJf "$WORK_DIR/ffmpeg.tar.xz" -C "$WORK_DIR/ffmpeg-extract"
FF_DIR="$(find "$WORK_DIR/ffmpeg-extract" -maxdepth 1 -type d -name 'ffmpeg-*linuxarm64*' | head -n 1)"
if [ -z "$FF_DIR" ]; then
  echo "Failed to locate extracted ffmpeg directory"
  exit 1
fi
cp "$FF_DIR/bin/ffmpeg" "$OUT_DIR/ffmpeg"
cp "$FF_DIR/bin/ffprobe" "$OUT_DIR/ffprobe"
chmod +x "$OUT_DIR/ffmpeg" "$OUT_DIR/ffprobe"

echo "=== Dependency build complete ==="
ls -lh "$OUT_DIR"
file "$OUT_DIR/yt-dlp" "$OUT_DIR/deno" "$OUT_DIR/ffmpeg" "$OUT_DIR/ffprobe" || true
