# YT Stream Module (Move Anything)

Experimental v2 sound-generator module that starts a YouTube audio stream on load.

## Current Behavior

- Exports a **v2 DSP plugin** (`move_plugin_init_v2`)
- On instance creation, launches a pipeline:
  - `yt-dlp` resolves an audio URL for a test video
  - `ffmpeg` decodes to 44.1kHz stereo `s16le`
- Audio is buffered and emitted in `render_block`

Default test URL:

- `https://www.youtube.com/watch?v=xvFZjo5PgG0`

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
