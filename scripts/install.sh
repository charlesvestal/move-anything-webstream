#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

if [ ! -f "$REPO_ROOT/dist/webstream-module.tar.gz" ]; then
  "$REPO_ROOT/scripts/build.sh"
fi

scp -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new \
  "$REPO_ROOT/dist/webstream-module.tar.gz" \
  ableton@move.local:~/move-anything/

ssh -o ConnectTimeout=8 ableton@move.local '
  set -e
  cd ~/move-anything
  mkdir -p modules/sound_generators
  tar -xzf webstream-module.tar.gz -C modules/sound_generators/
  rm -f webstream-module.tar.gz
  echo "Installed to ~/move-anything/modules/sound_generators/webstream"
'
