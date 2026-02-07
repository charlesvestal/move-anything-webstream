# Third-Party Notices

This repository contains original module code plus optional third-party runtime binaries.

## Scope

- The module code in this repository is licensed under MIT (see `src/module.json`).
- When built with bundled runtime dependencies, the output also includes third-party binaries with their own licenses and terms.
- Bundled dependency metadata and checksums are generated at build time in `build/deps/manifest.json` and included in release artifacts as `THIRD_PARTY_MANIFEST.json`.

## Bundled Runtime Dependencies

1. `yt-dlp`
- Upstream: <https://github.com/yt-dlp/yt-dlp>
- License: Unlicense
- Local license copy: `licenses/yt-dlp-UNLICENSE.txt`

2. `deno`
- Upstream: <https://github.com/denoland/deno>
- License: MIT
- Local license copy: `licenses/deno-MIT.txt`

3. `ffmpeg` / `ffprobe` (downloaded from yt-dlp FFmpeg builds)
- Upstream builds: <https://github.com/yt-dlp/FFmpeg-Builds>
- License profile depends on build variant; this project currently fetches the `gpl` arm64 build.
- Local GPL text copy: `licenses/GPL-3.0.txt`
- Additional FFmpeg licensing info: <https://www.ffmpeg.org/legal.html>

## Distribution Notes

- This project can build two release artifacts:
  - `yt-module.tar.gz` (bundled runtime binaries)
  - `yt-module-core.tar.gz` (core-only, runtime binaries are user-supplied)
- Distributors and users are responsible for complying with applicable licenses and the terms/policies of content sources.
- This project is a third-party community module and is not affiliated with or endorsed by Ableton, YouTube, or other content platforms.
