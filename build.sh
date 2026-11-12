#!/usr/bin/env bash
# Build both 128 KiB cartridges and, when the Store media exist, the Store bundle.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${PRG32_REPO:-$here/../PRG32}"
mkdir -p "$here/build" "$here/dist" "$here/store-bundle"
# sprites.h is generated and committed; regenerate it when Pillow is available.
if python3 -c "import PIL" 2>/dev/null; then python3 "$here/scripts/gen_sprites.py"
else echo "Pillow not available in $(command -v python3): using the committed sprites.h"; fi
# Piste tables from the map traces. With SKIPASS_MAP pointing at the map image
# (pdfimages -j of the piste map PDF) forest and lift crossings are resampled;
# without it the sampled data already in pistes.h are kept.
if [ -n "${SKIPASS_MAP:-}" ] && python3 -c "import PIL" 2>/dev/null; then
  python3 "$here/scripts/trace_pistes.py" --map "$SKIPASS_MAP"
else
  python3 "$here/scripts/trace_pistes.py" >/dev/null
fi
python3 "$here/scripts/generate_audio.py"
python3 "$here/scripts/store_manifest.py"
python3 "$sdk/tools/prg32audio_pack.py" "$here/assets/audio.json" --out "$here/build/skipass.audio"
media=1
for f in icon.png screenshot.png preview.mp4; do [ -f "$here/assets/$f" ] || media=0; done
for arch in esp32c6 qemu; do
  core="$here/build/skipass-madness-$arch-core.prg32"
  (cd "$sdk" && python3 -m prg32 cartridge build "$here/game.c" --portable --entry-prefix skipass_madness \
    --name "Skipass Madness" --architecture "$arch" --cart-ram-kib 128 \
    --required-feature audio --required-feature multiplayer --multiplayer \
    --build-dir "$here/build/$arch" --out "$core" --audio-block "$here/build/skipass.audio")
  if [ "$media" = 1 ]; then
    (cd "$sdk" && python3 -m prg32 store attach-metadata "$core" --metadata "$here/metadata.json" \
      --colophon "$here/colophon.json" --architecture "$arch" --icon "$here/assets/icon.png" \
      --screenshot "$here/assets/screenshot.png" --out "$here/dist/skipass-madness-$arch.prg32")
    cp "$here/dist/skipass-madness-$arch.prg32" "$here/store-bundle/"
  else
    cp "$core" "$here/dist/skipass-madness-$arch.prg32"
  fi
done
if [ "$media" = 1 ]; then
  cp "$here/manifest.json" "$here/colophon.json" "$here/assets/icon.png" "$here/assets/screenshot.png" \
     "$here/assets/preview.mp4" "$here/store-bundle/"
  version="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['version'])" "$here/manifest.json")"
  (cd "$sdk" && python3 -m prg32 store pack-bundle --manifest "$here/store-bundle/manifest.json" \
    --out "$here/dist/skipass-madness-$version.zip")
  echo "Store bundle: $here/dist/skipass-madness-$version.zip"
else
  echo "Store media missing (assets/icon.png, screenshot.png, preview.mp4): run scripts/capture_media.py, then rebuild"
fi
echo "Built 128 KiB cartridges in $here/dist"
