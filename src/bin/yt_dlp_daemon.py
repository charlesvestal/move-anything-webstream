#!/usr/bin/env python3
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request


def clean_field(value: object) -> str:
    if value is None:
        return ""
    s = str(value)
    s = s.replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()
    return s


def write_fields(*fields: object) -> None:
    line = "\t".join(clean_field(f) for f in fields)
    sys.stdout.write(f"{line}\n")
    sys.stdout.flush()


def normalize_provider(raw: str) -> str:
    p = (raw or "").strip().lower()
    if p in ("yt", "youtube"):
        return "youtube"
    if p in ("fs", "freesound"):
        return "freesound"
    if p in ("ia", "archive", "archiveorg", "internetarchive"):
        return "archive"
    if p in ("sc", "soundcloud"):
        return "soundcloud"
    return p


def setup_import_path() -> None:
    zip_path = ""
    if len(sys.argv) > 1:
        zip_path = sys.argv[1]
    if not zip_path:
        zip_path = os.path.join(os.path.dirname(__file__), "yt-dlp")
    if zip_path and os.path.exists(zip_path):
        sys.path.insert(0, zip_path)


def format_duration(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return clean_field(value)
    try:
        total = int(float(value))
    except Exception:
        return clean_field(value)
    if total < 0:
        total = 0
    h = total // 3600
    m = (total % 3600) // 60
    s = total % 60
    if h > 0:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m}:{s:02d}"


def parse_duration_seconds(value: object):
    if value is None:
        return None
    if isinstance(value, (int, float)):
        try:
            secs = float(value)
            if secs >= 0:
                return secs
            return None
        except Exception:
            return None

    raw = clean_field(value)
    if not raw:
        return None

    try:
        secs = float(raw)
        if secs >= 0:
            return secs
        return None
    except Exception:
        pass

    parts = raw.split(":")
    if len(parts) not in (2, 3):
        return None
    try:
        if len(parts) == 2:
            m = int(parts[0])
            s = int(float(parts[1]))
            total = m * 60 + s
        else:
            h = int(parts[0])
            m = int(parts[1])
            s = int(float(parts[2]))
            total = h * 3600 + m * 60 + s
    except Exception:
        return None
    if total < 0:
        return None
    return float(total)


def should_skip_search_entry(provider: str, entry: dict) -> bool:
    if not isinstance(entry, dict):
        return True

    p = normalize_provider(provider)
    if p != "soundcloud":
        return False

    # SoundCloud Go / preview-only search results commonly appear as exactly
    # ~30-second entries and resolve to *_preview formats.
    secs = parse_duration_seconds(entry.get("duration"))
    if secs is None:
        secs = parse_duration_seconds(entry.get("duration_string"))
    if secs is not None and 29.0 <= secs <= 31.0:
        return True

    return False


def create_ytdlp_search_opts(provider: str, limit: int) -> dict:
    opts = {
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
        "extract_flat": True,
        "playlistend": int(limit),
        "noplaylist": True,
    }
    if provider == "youtube":
        opts["extractor_args"] = {"youtube": {"player_skip": ["js"]}}
    return opts


def create_ytdlp_resolve_opts(provider: str) -> dict:
    opts = {
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
        "noplaylist": True,
        "format": "bestaudio[ext=m4a]/bestaudio",
    }
    if provider == "youtube":
        opts["extractor_args"] = {"youtube": {"player_skip": ["js"]}}
    if provider == "soundcloud":
        opts["format"] = "http_mp3_1_0/hls_mp3_1_0/bestaudio"
    return opts


def ensure_ytdlp(yt_dlp_mod):
    if yt_dlp_mod is None:
        raise RuntimeError("yt-dlp is unavailable")


def search_request_ytdlp(yt_dlp_mod, provider: str, limit_text: str, query: str) -> None:
    ensure_ytdlp(yt_dlp_mod)

    try:
        limit = int(limit_text)
    except Exception:
        limit = 20
    if limit < 1:
        limit = 1
    if limit > 50:
        limit = 50

    if provider == "youtube":
        expr = f"ytsearch{limit}:{query}"
    elif provider == "soundcloud":
        expr = f"scsearch{limit}:{query}"
    else:
        raise RuntimeError(f"unsupported yt-dlp provider: {provider}")

    with yt_dlp_mod.YoutubeDL(create_ytdlp_search_opts(provider, limit)) as ydl:
        data = ydl.extract_info(expr, download=False)

    entries = []
    if isinstance(data, dict):
        entries = data.get("entries") or []

    count = 0
    write_fields("SEARCH_BEGIN")
    for entry in entries:
        if should_skip_search_entry(provider, entry):
            continue
        sid = clean_field(entry.get("id") or "")
        title = clean_field(entry.get("title") or "")
        channel = clean_field(entry.get("channel") or entry.get("uploader") or "")
        duration = entry.get("duration_string")
        if not duration:
            duration = format_duration(entry.get("duration"))

        url = clean_field(entry.get("webpage_url") or "")
        if not url and provider == "youtube" and sid:
            url = f"https://www.youtube.com/watch?v={sid}"
        if not sid and url:
            sid = url

        if not title or not url:
            continue

        write_fields("SEARCH_ITEM", sid, title, channel, duration, url)
        count += 1

    write_fields("SEARCH_END", str(count))


def resolve_request_ytdlp(yt_dlp_mod, provider: str, source_url: str) -> None:
    ensure_ytdlp(yt_dlp_mod)

    with yt_dlp_mod.YoutubeDL(create_ytdlp_resolve_opts(provider)) as ydl:
        data = ydl.extract_info(source_url, download=False)

    if isinstance(data, dict) and isinstance(data.get("entries"), list) and data.get("entries"):
        first = data["entries"][0]
        if isinstance(first, dict):
            data = first

    if not isinstance(data, dict):
        write_fields("ERROR", "resolve returned invalid payload")
        return

    media_url = data.get("url") or ""
    if not media_url:
        formats = data.get("formats")
        if isinstance(formats, list):
            for f in formats:
                if not isinstance(f, dict):
                    continue
                candidate = f.get("url")
                if candidate:
                    media_url = candidate
                    break

    if not media_url:
        write_fields("ERROR", "resolve returned empty media url")
        return

    headers = data.get("http_headers") or {}
    user_agent = ""
    referer = ""
    if isinstance(headers, dict):
        user_agent = headers.get("User-Agent") or ""
        referer = headers.get("Referer") or ""
    write_fields("RESOLVE_OK", media_url, user_agent, referer)


def load_provider_config() -> dict:
    path = os.environ.get(
        "WEBSTREAM_PROVIDER_CONFIG",
        "/data/UserData/move-anything/config/webstream_providers.json",
    )
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                return data
    except Exception:
        pass
    return {}


def config_provider_block(config: dict, provider: str):
    if not isinstance(config, dict):
        return {}
    providers = config.get("providers")
    if isinstance(providers, dict):
        block = providers.get(provider)
        if isinstance(block, dict):
            return block
    direct = config.get(provider)
    if isinstance(direct, dict):
        return direct
    return {}


def provider_is_enabled(provider: str, config: dict) -> bool:
    block = config_provider_block(config, provider)
    enabled = block.get("enabled")
    if isinstance(enabled, bool):
        return enabled
    return True


def freesound_api_key(config: dict) -> str:
    env_key = os.environ.get("FREESOUND_API_KEY") or os.environ.get("FREESOUND_TOKEN")
    if env_key:
        return env_key.strip()

    block = config_provider_block(config, "freesound")
    for key in ("api_key", "token"):
        value = block.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()

    flat = config.get("freesound_api_key")
    if isinstance(flat, str) and flat.strip():
        return flat.strip()

    return ""


def http_json(url: str, timeout: int = 20):
    req = urllib.request.Request(url, headers={"User-Agent": "move-anything-webstream/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            body = exc.read().decode("utf-8", errors="replace")
            parsed = json.loads(body)
            if isinstance(parsed, dict):
                detail = parsed.get("detail") or parsed.get("error") or ""
        except Exception:
            detail = ""
        if detail:
            raise RuntimeError(f"HTTP {exc.code}: {detail}")
        raise RuntimeError(f"HTTP {exc.code}")
    except Exception as exc:
        raise RuntimeError(str(exc))

    try:
        return json.loads(data.decode("utf-8", errors="replace"))
    except Exception as exc:
        raise RuntimeError(f"invalid JSON response: {exc}")


def extract_freesound_id(source: str) -> str:
    s = (source or "").strip()
    if s.isdigit():
        return s
    m = re.search(r"/(?:s|sounds)/(\d+)", s)
    if m:
        return m.group(1)
    m = re.search(r"(\d+)", s)
    if m:
        return m.group(1)
    return ""


def search_request_freesound(limit_text: str, query: str, config: dict) -> None:
    key = freesound_api_key(config)
    if not key:
        raise RuntimeError("freesound API key missing (set FREESOUND_API_KEY or config)")

    try:
        limit = int(limit_text)
    except Exception:
        limit = 20
    if limit < 1:
        limit = 1
    if limit > 50:
        limit = 50

    params = {
        "query": query,
        "fields": "id,name,username,duration",
        "page_size": str(limit),
        "token": key,
    }
    url = "https://freesound.org/apiv2/search/text/?" + urllib.parse.urlencode(params)
    data = http_json(url, timeout=20)
    results = []
    if isinstance(data, dict):
        results = data.get("results") or []

    count = 0
    write_fields("SEARCH_BEGIN")
    for entry in results:
        if not isinstance(entry, dict):
            continue
        sid = clean_field(entry.get("id") or "")
        title = clean_field(entry.get("name") or "")
        channel = clean_field(entry.get("username") or "")
        duration = format_duration(entry.get("duration"))
        if not sid or not title:
            continue
        url = f"https://freesound.org/s/{sid}/"
        write_fields("SEARCH_ITEM", sid, title, channel, duration, url)
        count += 1

    write_fields("SEARCH_END", str(count))


def resolve_request_freesound(source_url: str, config: dict) -> None:
    key = freesound_api_key(config)
    if not key:
        raise RuntimeError("freesound API key missing (set FREESOUND_API_KEY or config)")

    sid = extract_freesound_id(source_url)
    if not sid:
        raise RuntimeError("could not parse freesound id")

    params = {"fields": "id,name,previews", "token": key}
    url = f"https://freesound.org/apiv2/sounds/{urllib.parse.quote(sid)}/?" + urllib.parse.urlencode(params)
    data = http_json(url, timeout=20)
    if not isinstance(data, dict):
        raise RuntimeError("freesound resolve returned invalid payload")

    previews = data.get("previews") or {}
    if not isinstance(previews, dict):
        previews = {}

    media_url = ""
    for k in ("preview-hq-mp3", "preview-lq-mp3", "preview-hq-ogg", "preview-lq-ogg"):
        candidate = previews.get(k)
        if isinstance(candidate, str) and candidate:
            media_url = candidate
            break

    if not media_url:
        raise RuntimeError("freesound preview url missing")

    write_fields("RESOLVE_OK", media_url, "", "")


def extract_archive_identifier(source: str) -> str:
    s = (source or "").strip()
    if not s:
        return ""
    if "/" not in s and " " not in s and "\t" not in s:
        return s

    try:
        parsed = urllib.parse.urlparse(s)
    except Exception:
        parsed = None

    if parsed and parsed.path:
        parts = [p for p in parsed.path.split("/") if p]
        if len(parts) >= 2 and parts[0] in ("details", "download"):
            return parts[1]
        if parts:
            return parts[0]

    m = re.search(r"archive\.org/(?:details|download)/([^/?#]+)", s)
    if m:
        return m.group(1)
    return ""


def archive_file_score(file_info: dict):
    name = str(file_info.get("name") or "")
    if not name or name.endswith("/"):
        return None

    ext = os.path.splitext(name)[1].lower()
    fmt = str(file_info.get("format") or "").lower()
    source = str(file_info.get("source") or "").lower()

    blocked = {
        ".txt", ".xml", ".json", ".jpg", ".jpeg", ".png", ".gif", ".pdf", ".m3u", ".torrent", ".sqlite", ".db"
    }
    if ext in blocked:
        return None

    if ext == ".mp3" or "mp3" in fmt:
        score = 10
    elif ext in (".ogg", ".oga", ".opus") or "ogg" in fmt or "opus" in fmt:
        score = 20
    elif ext in (".m4a", ".aac", ".mp4") or "aac" in fmt or "m4a" in fmt:
        score = 30
    elif ext in (".flac", ".wav", ".aiff", ".aif") or "flac" in fmt or "wave" in fmt or "wav" in fmt:
        score = 60
    else:
        return None

    if source == "derivative":
        score -= 3

    return score


def pick_archive_media_file(files: list) -> str:
    best_name = ""
    best_score = None
    for f in files:
        if not isinstance(f, dict):
            continue
        score = archive_file_score(f)
        if score is None:
            continue
        name = str(f.get("name") or "")
        if best_score is None or score < best_score or (score == best_score and name < best_name):
            best_score = score
            best_name = name
    return best_name


def search_request_archive(limit_text: str, query: str) -> None:
    try:
        limit = int(limit_text)
    except Exception:
        limit = 20
    if limit < 1:
        limit = 1
    if limit > 50:
        limit = 50

    params = {
        "q": f"mediatype:audio AND ({query})",
        "fl[]": ["identifier", "title", "creator", "publicdate"],
        "sort[]": ["downloads desc"],
        "rows": str(limit),
        "page": "1",
        "output": "json",
    }
    url = "https://archive.org/advancedsearch.php?" + urllib.parse.urlencode(params, doseq=True)
    data = http_json(url, timeout=20)

    docs = []
    if isinstance(data, dict):
        response = data.get("response") or {}
        if isinstance(response, dict):
            docs = response.get("docs") or []

    count = 0
    write_fields("SEARCH_BEGIN")
    for entry in docs:
        if not isinstance(entry, dict):
            continue

        sid = clean_field(entry.get("identifier") or "")
        title = clean_field(entry.get("title") or sid)
        creator = entry.get("creator") or ""
        if isinstance(creator, list):
            creator = creator[0] if creator else ""
        channel = clean_field(creator)

        if not sid or not title:
            continue

        detail_url = f"https://archive.org/details/{sid}"
        write_fields("SEARCH_ITEM", sid, title, channel, "", detail_url)
        count += 1

    write_fields("SEARCH_END", str(count))


def resolve_request_archive(source_url: str) -> None:
    identifier = extract_archive_identifier(source_url)
    if not identifier:
        raise RuntimeError("could not parse archive.org identifier")

    metadata_url = f"https://archive.org/metadata/{urllib.parse.quote(identifier)}"
    data = http_json(metadata_url, timeout=20)
    if not isinstance(data, dict):
        raise RuntimeError("archive metadata returned invalid payload")

    files = data.get("files") or []
    if not isinstance(files, list):
        files = []

    name = pick_archive_media_file(files)
    if not name:
        raise RuntimeError("archive item has no supported audio file")

    media_url = "https://archive.org/download/{}/{}".format(
        urllib.parse.quote(identifier),
        urllib.parse.quote(name, safe="/"),
    )
    write_fields("RESOLVE_OK", media_url, "", "")


def search_request(yt_dlp_mod, provider: str, limit_text: str, query: str) -> None:
    provider = normalize_provider(provider)
    config = load_provider_config()
    if not provider_is_enabled(provider, config):
        raise RuntimeError(f"provider disabled: {provider}")

    if provider in ("youtube", "soundcloud"):
        search_request_ytdlp(yt_dlp_mod, provider, limit_text, query)
        return
    if provider == "freesound":
        search_request_freesound(limit_text, query, config)
        return
    if provider == "archive":
        search_request_archive(limit_text, query)
        return

    raise RuntimeError(f"unsupported provider: {provider}")


def resolve_request(yt_dlp_mod, provider: str, source_url: str) -> None:
    provider = normalize_provider(provider)
    config = load_provider_config()
    if not provider_is_enabled(provider, config):
        raise RuntimeError(f"provider disabled: {provider}")

    if provider in ("youtube", "soundcloud"):
        resolve_request_ytdlp(yt_dlp_mod, provider, source_url)
        return
    if provider == "freesound":
        resolve_request_freesound(source_url, config)
        return
    if provider == "archive":
        resolve_request_archive(source_url)
        return

    raise RuntimeError(f"unsupported provider: {provider}")


def parse_search_parts(parts: list):
    if len(parts) >= 4:
        provider = parts[1]
        limit_text = parts[2]
        query = "\t".join(parts[3:])
        return provider, limit_text, query
    if len(parts) >= 3:
        provider = "youtube"
        limit_text = parts[1]
        query = "\t".join(parts[2:])
        return provider, limit_text, query
    raise RuntimeError("SEARCH requires provider+limit+query")


def parse_resolve_parts(parts: list):
    if len(parts) >= 3:
        provider = parts[1]
        source_url = "\t".join(parts[2:])
        return provider, source_url
    if len(parts) >= 2:
        provider = "youtube"
        source_url = "\t".join(parts[1:])
        return provider, source_url
    raise RuntimeError("RESOLVE requires provider+source url")


def main() -> int:
    setup_import_path()

    yt_dlp_mod = None
    try:
        import yt_dlp as imported  # type: ignore
        yt_dlp_mod = imported
    except Exception:
        yt_dlp_mod = None

    write_fields("READY")

    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line:
            continue
        parts = line.split("\t")
        cmd = parts[0]

        try:
            if cmd == "PING":
                write_fields("PONG")
            elif cmd == "SEARCH":
                provider, limit_text, query = parse_search_parts(parts)
                search_request(yt_dlp_mod, provider, limit_text, query)
            elif cmd == "RESOLVE":
                provider, source_url = parse_resolve_parts(parts)
                resolve_request(yt_dlp_mod, provider, source_url)
            elif cmd == "QUIT":
                write_fields("BYE")
                break
            else:
                write_fields("ERROR", f"unknown command: {cmd}")
        except Exception as exc:
            write_fields("ERROR", f"{exc}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
