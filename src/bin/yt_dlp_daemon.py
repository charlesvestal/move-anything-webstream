#!/usr/bin/env python3
import os
import sys


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


def setup_import_path() -> None:
    zip_path = ""
    if len(sys.argv) > 1:
        zip_path = sys.argv[1]
    if not zip_path:
        zip_path = os.path.join(os.path.dirname(__file__), "yt-dlp")
    if zip_path and os.path.exists(zip_path):
        sys.path.insert(0, zip_path)


def create_search_opts(limit: int) -> dict:
    return {
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
        "extract_flat": True,
        "playlistend": int(limit),
        "noplaylist": True,
        "extractor_args": {"youtube": {"player_skip": ["js"]}},
    }


def create_resolve_opts() -> dict:
    return {
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
        "format": "bestaudio[ext=m4a]/bestaudio",
        "noplaylist": True,
        "extractor_args": {"youtube": {"player_skip": ["js"]}},
    }


def search_request(yt_dlp_mod, limit_text: str, query: str) -> None:
    try:
        limit = int(limit_text)
    except Exception:
        limit = 20
    if limit < 1:
        limit = 1
    if limit > 50:
        limit = 50

    ydl_opts = create_search_opts(limit)
    with yt_dlp_mod.YoutubeDL(ydl_opts) as ydl:
        data = ydl.extract_info(f"ytsearch{limit}:{query}", download=False)

    entries = []
    if isinstance(data, dict):
        entries = data.get("entries") or []
    write_fields("SEARCH_BEGIN")
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        write_fields(
            "SEARCH_ITEM",
            entry.get("id", ""),
            entry.get("title", ""),
            entry.get("channel", ""),
            entry.get("duration_string", ""),
        )
    write_fields("SEARCH_END", str(len(entries)))


def resolve_request(yt_dlp_mod, source_url: str) -> None:
    ydl_opts = create_resolve_opts()
    with yt_dlp_mod.YoutubeDL(ydl_opts) as ydl:
        data = ydl.extract_info(source_url, download=False)
    if not isinstance(data, dict):
        write_fields("ERROR", "resolve returned invalid payload")
        return
    media_url = data.get("url") or ""
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


def main() -> int:
    setup_import_path()
    try:
        import yt_dlp  # type: ignore
    except Exception as exc:
        write_fields("ERROR", f"import yt_dlp failed: {exc}")
        return 1

    write_fields("READY")

    for raw in sys.stdin:
        line = raw.rstrip("\r\n")
        if not line:
            continue
        parts = line.split("\t", 2)
        cmd = parts[0]
        try:
            if cmd == "PING":
                write_fields("PONG")
            elif cmd == "SEARCH":
                if len(parts) < 3:
                    write_fields("ERROR", "SEARCH requires limit and query")
                    continue
                search_request(yt_dlp, parts[1], parts[2])
            elif cmd == "RESOLVE":
                if len(parts) < 2:
                    write_fields("ERROR", "RESOLVE requires source url")
                    continue
                source_url = parts[1] if len(parts) == 2 else parts[1] + "\t" + parts[2]
                resolve_request(yt_dlp, source_url)
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
