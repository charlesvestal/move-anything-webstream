# Building move-anything-webstream

This module is a **v2 DSP plugin** that relies on three runtime binaries bundled with the module:

- `yt-dlp` (zipimport binary built from source)
- `deno` (JavaScript runtime for YouTube extraction compatibility)
- `ffmpeg` / `ffprobe` (audio decode/transcode)

Target runtime: Ableton Move (`aarch64` Linux, glibc)

## Output Layout

Build output in `dist/webstream/`:

- `module.json`
- `ui.js`
- `dsp.so`
- `bin/yt-dlp` (optional, if deps built)
- `bin/yt_dlp_daemon.py` (always bundled; warm helper process for yt-dlp)
- `bin/deno` (optional)
- `bin/ffmpeg` (optional)
- `bin/ffprobe` (optional)
- `THIRD_PARTY_NOTICES.md`
- `licenses/`
- `THIRD_PARTY_MANIFEST.json` (if generated)

Release tarball:

- `dist/webstream-module.tar.gz`
- `dist/webstream-module-core.tar.gz` (core-only variant, for release builds)

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
- `build/deps/manifest.json` (version/source/checksum metadata)

## 5) Build module (v2 plugin)

```bash
./scripts/build.sh
```

What it does:

- Cross-compiles `src/dsp/yt_stream_plugin.c` to `dsp.so` for `aarch64`
- Packages module files to `dist/webstream/`
- If `build/deps/bin` exists, bundles runtime binaries into `dist/webstream/bin/`
- Copies third-party notices/licenses into the module
- Creates `dist/webstream-module.tar.gz` by default

Force profile selection:

```bash
BUNDLE_RUNTIME=with-deps ./scripts/build.sh
BUNDLE_RUNTIME=core-only ./scripts/build.sh
```

Build both release assets:

```bash
./scripts/build-deps.sh
./scripts/build-release-assets.sh
```

## 6) Deploy to Move

```bash
./scripts/install.sh
```

Manual deploy equivalent:

```bash
scp dist/webstream-module.tar.gz ableton@move.local:~/move-anything/
ssh ableton@move.local '
  cd ~/move-anything &&
  mkdir -p modules/sound_generators &&
  tar -xzf webstream-module.tar.gz -C modules/sound_generators/
'
```

## Validation Checklist

On Move:

```bash
ssh ableton@move.local '
  ls -l ~/move-anything/modules/sound_generators/webstream/dsp.so
  ls -l ~/move-anything/modules/sound_generators/webstream/bin/yt-dlp
  ls -l ~/move-anything/modules/sound_generators/webstream/bin/deno
  ls -l ~/move-anything/modules/sound_generators/webstream/bin/ffmpeg
'
```

Optional architecture check:

```bash
file dist/webstream/dsp.so
# Expect: ELF 64-bit LSB shared object, ARM aarch64
```

## Notes

- The plugin is search-driven (it does not auto-start a hardcoded URL on load).
- Runtime network and YouTube behavior can affect startup latency.
- `yt-dlp`/site extraction behavior may change over time; rebuild dependencies as needed.
- Provider support is mixed:
  - `youtube` + `soundcloud` use `yt-dlp`.
  - `archive` uses archive.org public APIs.
  - `freesound` uses FreeSound API and requires a token (`FREESOUND_API_KEY`/`FREESOUND_TOKEN` env var or `webstream_providers.json`).
- Optional provider config file on Move:
  - `/data/UserData/move-anything/config/webstream_providers.json`
- For distribution terms and dependency licenses, see `THIRD_PARTY_NOTICES.md`.
