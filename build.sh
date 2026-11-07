#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${PRG32_REPO:-$here/../PRG32}"
mkdir -p "$here/build" "$here/dist"
python3 "$sdk/tools/prg32audio_pack.py" "$here/audio.json" --out "$here/build/skipass.audio"
for arch in esp32c6 qemu; do
  (cd "$sdk" && python3 -m prg32 cartridge build "$here/game.c" --portable --entry-prefix skipass_madness --name "Skipass Madness" --architecture "$arch" --cart-ram-kib 128 --required-feature audio --required-feature multiplayer --multiplayer --build-dir "$here/build/$arch" --out "$here/dist/skipass-madness-$arch.prg32" --audio-block "$here/build/skipass.audio")
done
echo "Built 128 KiB cartridges in $here/dist"
