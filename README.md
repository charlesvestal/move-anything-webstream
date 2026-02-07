# YT Stream Module (Move Anything)

Experimental v2 sound-generator module for YouTube search and streamed audio playback.

## Current Behavior

- Exports a **v2 DSP plugin** (`move_plugin_init_v2`)
- Uses menu UI with:
  - `[New Search...]`
  - `[Previous searches]`
  - search results list
- Starts streaming when a result is selected
- Uses `yt-dlp` + `ffmpeg` pipeline to decode to 44.1kHz stereo `s16le`
- Supports transport controls (play/pause, seek ±15s, stop, restart) via mapped knobs

## Build

```bash
./scripts/build-deps.sh   # optional but recommended: bundle yt-dlp/deno/ffmpeg
./scripts/build.sh
```

## Install

```bash
./scripts/install.sh
```

## Dependency Build Documentation

See `BUILDING.md` for detailed, reproducible steps for `yt-dlp`, `deno`, and `ffmpeg`.
