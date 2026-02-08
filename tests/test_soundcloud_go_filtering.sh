#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DAEMON_PY="$ROOT_DIR/src/bin/yt_dlp_daemon.py"

if [[ ! -f "$DAEMON_PY" ]]; then
  echo "FAIL: missing daemon script at $DAEMON_PY"
  exit 1
fi

python3 - <<'PY'
import importlib.util
from pathlib import Path

daemon_path = Path("src/bin/yt_dlp_daemon.py")
spec = importlib.util.spec_from_file_location("yt_dlp_daemon", daemon_path)
if spec is None or spec.loader is None:
    raise SystemExit("FAIL: unable to import yt_dlp_daemon.py")

mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

fn = getattr(mod, "should_skip_search_entry", None)
if fn is None:
    raise SystemExit("FAIL: should_skip_search_entry() missing")

if fn("soundcloud", {"title": "Hard Times", "duration": 30.0}) is not True:
    raise SystemExit("FAIL: soundcloud 30s preview-like entries should be filtered")

if fn("soundcloud", {"title": "Good Kid", "duration": 182.739}) is not False:
    raise SystemExit("FAIL: normal soundcloud tracks should remain")

if fn("youtube", {"title": "30 second tutorial", "duration": 30.0}) is not False:
    raise SystemExit("FAIL: non-soundcloud providers should not be filtered by this rule")

if fn("soundcloud", {"title": "Something", "duration_string": "0:30"}) is not True:
    raise SystemExit("FAIL: soundcloud duration_string preview signature should be filtered")

print("PASS: SoundCloud preview filtering behavior is correct")
PY
