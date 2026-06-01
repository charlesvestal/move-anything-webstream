# Webstream Module (Move Everything)

Experimental v2 sound-generator module for web audio playback via provider backends.

Third-party, unsupported community module. Not affiliated with or endorsed by Ableton or YouTube.

## Current Behavior

- Exports a **v2 DSP plugin** (`move_plugin_init_v2`)
- Uses menu UI with:
  - `[New Search...]` (provider picker first)
  - `[Previous searches]`
  - search results list
- Starts streaming when a result is selected
- Uses a warm `yt-dlp` daemon for search/URL resolve, then `ffmpeg` decode to 44.1kHz stereo `s16le`
- Supports transport controls (play/pause, seek ±15s, stop, restart) via mapped knobs
- Current providers:
  - `youtube` (via `yt-dlp`)
  - `soundcloud` (via `yt-dlp`)
  - `archive` (via archive.org APIs)
  - `freesound` (via FreeSound API; requires token)
  - `cratedig` — random music discovery via Discogs API (optional token)

## Provider Configuration

Runtime provider config path:

- `/data/UserData/move-anything/config/webstream_providers.json`

Example:

```json
{
  "providers": {
    "freesound": {
      "enabled": true,
      "api_key": "YOUR_FREESOUND_TOKEN"
    },
    "archive": { "enabled": true },
    "youtube": { "enabled": true },
    "soundcloud": { "enabled": true },
    "cratedig": {
      "token": "YOUR_DISCOGS_PERSONAL_TOKEN"
    }
  }
}
```

The FreeSound API key can be set under **FreeSound API Key** on the Webstream module page in the Schwung manager (recommended; stored as a secret at `<module_dir>/secrets/freesound_api_key.txt`), via `webstream_providers.json` (above), or via env var:

- `FREESOUND_API_KEY` (or `FREESOUND_TOKEN`)

### Crate Dig (Discogs)

Crate Dig discovers random music by searching [Discogs](https://www.discogs.com/) for releases with YouTube videos. Filter by genre, style, decade, and country.

Works without a token (25 requests/min per device). For higher limits (60/min), add a free Discogs personal access token:

1. Create a Discogs account at [discogs.com](https://www.discogs.com/)
2. Go to [Settings → Developers](https://www.discogs.com/settings/developers)
3. Click "Generate new token"
4. Enter it under **Discogs Token** on the Webstream module page in the Schwung manager (recommended), or add it to `webstream_providers.json` (see above), or set the `DISCOGS_TOKEN` env var

The Schwung manager stores the token as a secret at `<module_dir>/secrets/discogs_token.txt` (mode 0600), declared via the module's `settings-schema.json`. It takes precedence over `webstream_providers.json` and `DISCOGS_TOKEN`.

Search results provided by [Discogs](https://www.discogs.com/).

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

- `dist/webstream-module.tar.gz` bundles runtime dependencies.
- `dist/webstream-module-core.tar.gz` is core-only (user supplies `yt-dlp`/`deno`/`ffmpeg`).

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

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
