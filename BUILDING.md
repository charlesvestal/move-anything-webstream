# Building move-anything-yt

This module is a **v2 DSP plugin** that relies on three runtime binaries bundled with the module:

- `yt-dlp` (zipimport binary built from source)
- `deno` (JavaScript runtime for YouTube extraction compatibility)
- `ffmpeg` / `ffprobe` (audio decode/transcode)

Target runtime: Ableton Move (`aarch64` Linux, glibc)

## Output Layout

Build output in `dist/yt/`:

- `module.json`
- `ui.js`
- `dsp.so`
- `bin/yt-dlp` (optional, if deps built)
- `bin/deno` (optional)
- `bin/ffmpeg` (optional)
- `bin/ffprobe` (optional)

Release tarball:

- `dist/yt-module.tar.gz`

## 1) Build yt-dlp for Move

We build the Unix zipimport executable directly from source and enable lazy extractors for faster startup.

```bash
git clone --depth 1 https://github.com/yt-dlp/yt-dlp.git /tmp/yt-dlp-src
cd /tmp/yt-dlp-src
make clean || true
make lazy-extractors yt-dlp
./yt-dlp --version
```

Expected artifact:

- `/tmp/yt-dlp-src/yt-dlp` (Python shebang executable)

Why this method:

- Keeps behavior close to upstream
- `lazy-extractors` significantly improves startup on Move-class CPUs

## 2) Fetch deno (aarch64)

```bash
DENO_VERSION=$(python3 - <<'PY'
import json, urllib.request
with urllib.request.urlopen('https://api.github.com/repos/denoland/deno/releases/latest', timeout=30) as r:
    print(json.load(r)['tag_name'])
PY
)
curl -fL -o deno.zip \
  "https://github.com/denoland/deno/releases/download/${DENO_VERSION}/deno-aarch64-unknown-linux-gnu.zip"
unzip -o deno.zip deno
chmod +x deno
./deno --version
```

Expected artifact:

- `./deno` (aarch64 ELF)

## 3) Fetch ffmpeg/ffprobe (aarch64)

Use yt-dlp’s FFmpeg builds to match extractor expectations:

```bash
curl -fL -o ffmpeg-arm64.tar.xz \
  "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linuxarm64-gpl.tar.xz"
tar -xJf ffmpeg-arm64.tar.xz
FFDIR=$(find . -maxdepth 1 -type d -name 'ffmpeg-*linuxarm64*' | head -n1)
"$FFDIR/bin/ffmpeg" -version | head -n1
"$FFDIR/bin/ffprobe" -version | head -n1
```

Expected artifacts:

- `.../bin/ffmpeg`
- `.../bin/ffprobe`

## 4) Automated dependency bundling in this repo

Run:

```bash
./scripts/build-deps.sh
```

This script creates:

- `build/deps/bin/yt-dlp`
- `build/deps/bin/deno`
- `build/deps/bin/ffmpeg`
- `build/deps/bin/ffprobe`

## 5) Build module (v2 plugin)

```bash
./scripts/build.sh
```

What it does:

- Cross-compiles `src/dsp/yt_stream_plugin.c` to `dsp.so` for `aarch64`
- Packages module files to `dist/yt/`
- If `build/deps/bin` exists, bundles runtime binaries into `dist/yt/bin/`
- Creates `dist/yt-module.tar.gz`

## 6) Deploy to Move

```bash
./scripts/install.sh
```

Manual deploy equivalent:

```bash
scp dist/yt-module.tar.gz ableton@move.local:~/move-anything/
ssh ableton@move.local '
  cd ~/move-anything &&
  mkdir -p modules/sound_generators &&
  tar -xzf yt-module.tar.gz -C modules/sound_generators/
'
```

## Validation Checklist

On Move:

```bash
ssh ableton@move.local '
  ls -l ~/move-anything/modules/sound_generators/yt/dsp.so
  ls -l ~/move-anything/modules/sound_generators/yt/bin/yt-dlp
  ls -l ~/move-anything/modules/sound_generators/yt/bin/deno
  ls -l ~/move-anything/modules/sound_generators/yt/bin/ffmpeg
'
```

Optional architecture check:

```bash
file dist/yt/dsp.so
# Expect: ELF 64-bit LSB shared object, ARM aarch64
```

## Notes

- The plugin is currently hard-wired to a test URL via module defaults.
- Runtime network and YouTube behavior can affect startup latency.
- `yt-dlp`/site extraction behavior may change over time; rebuild dependencies as needed.
