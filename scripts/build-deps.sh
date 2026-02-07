#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$REPO_ROOT/build/deps/bin"
WORK_DIR="$REPO_ROOT/build/deps/work"
MANIFEST_PATH="$REPO_ROOT/build/deps/manifest.json"

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
DENO_URL="https://github.com/denoland/deno/releases/download/${DENO_VERSION}/deno-aarch64-unknown-linux-gnu.zip"
curl -fL -o "$WORK_DIR/deno.zip" "$DENO_URL"
unzip -o "$WORK_DIR/deno.zip" -d "$WORK_DIR/deno" >/dev/null
cp "$WORK_DIR/deno/deno" "$OUT_DIR/deno"
chmod +x "$OUT_DIR/deno"


echo "=== Downloading ffmpeg (yt-dlp arm64 build) ==="
FFMPEG_URL="https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz"
curl -fL -o "$WORK_DIR/ffmpeg.tar.xz" "$FFMPEG_URL"
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

echo "=== Writing third-party dependency manifest ==="
YTDLP_COMMIT="$(cd "$YTDLP_DIR" && git rev-parse HEAD 2>/dev/null || true)"
YTDLP_VERSION_STR="$("$OUT_DIR/yt-dlp" --version 2>/dev/null | head -n1 | tr -d '\r')"
DENO_VERSION_STR="$DENO_VERSION"
FFMPEG_VERSION_STR="$(basename "$FF_DIR")"
python3 - "$OUT_DIR" "$MANIFEST_PATH" "$YTDLP_COMMIT" "$DENO_VERSION" "$DENO_URL" "$FFMPEG_URL" "$YTDLP_VERSION_STR" "$DENO_VERSION_STR" "$FFMPEG_VERSION_STR" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

out_dir, manifest_path, ytdlp_commit, deno_tag, deno_url, ffmpeg_url, ytdlp_version, deno_version, ffmpeg_version = sys.argv[1:]

def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

manifest = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "artifacts": {
        "yt-dlp": {
            "version": ytdlp_version,
            "source_repo": "https://github.com/yt-dlp/yt-dlp",
            "source_ref": ytdlp_commit,
            "license": "Unlicense",
            "sha256": sha256(os.path.join(out_dir, "yt-dlp")),
        },
        "deno": {
            "version": deno_version,
            "source_url": deno_url,
            "source_tag": deno_tag,
            "license": "MIT",
            "sha256": sha256(os.path.join(out_dir, "deno")),
        },
        "ffmpeg": {
            "version": ffmpeg_version,
            "source_url": ffmpeg_url,
            "license": "GPL-3.0-or-later (build-dependent)",
            "sha256": sha256(os.path.join(out_dir, "ffmpeg")),
        },
        "ffprobe": {
            "version": ffmpeg_version,
            "source_url": ffmpeg_url,
            "license": "GPL-3.0-or-later (build-dependent)",
            "sha256": sha256(os.path.join(out_dir, "ffprobe")),
        },
    },
}

os.makedirs(os.path.dirname(manifest_path), exist_ok=True)
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY

echo "=== Dependency build complete ==="
ls -lh "$OUT_DIR"
file "$OUT_DIR/yt-dlp" "$OUT_DIR/deno" "$OUT_DIR/ffmpeg" "$OUT_DIR/ffprobe" || true
echo "Manifest: $MANIFEST_PATH"
