# YT Stream Module (Move Anything)

Experimental v2 sound-generator module for streamed audio playback via `yt-dlp` backends.

Third-party, unsupported community module. Not affiliated with or endorsed by Ableton or YouTube.

## Current Behavior

- Exports a **v2 DSP plugin** (`move_plugin_init_v2`)
- Uses menu UI with:
  - `[New Search...]`
  - `[Previous searches]`
  - search results list
- Starts streaming when a result is selected
- Uses a warm `yt-dlp` daemon for search/URL resolve, then `ffmpeg` decode to 44.1kHz stereo `s16le`
- Supports transport controls (play/pause, seek ±15s, stop, restart) via mapped knobs

## Build

```bash
./scripts/build-deps.sh   # optional but recommended: bundle yt-dlp/deno/ffmpeg
./scripts/build.sh
```

Release assets (both variants):

```bash
./scripts/build-deps.sh
./scripts/build-release-assets.sh
```

- `dist/yt-module.tar.gz` bundles runtime dependencies.
- `dist/yt-module-core.tar.gz` is core-only (user supplies `yt-dlp`/`deno`/`ffmpeg`).

## Install

```bash
./scripts/install.sh
```

## Dependency Build Documentation

See `BUILDING.md` for detailed, reproducible steps for `yt-dlp`, `deno`, and `ffmpeg`.

## Notices

- Third-party notices: `THIRD_PARTY_NOTICES.md`
- License copies: `licenses/`
- Users are responsible for complying with source-platform terms and content rights.
