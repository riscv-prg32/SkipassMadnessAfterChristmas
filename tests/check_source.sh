#!/usr/bin/env sh
# Source policy checks for the portable cartridge, plus the host harness
# (autopilot descent of every piste and a sanitizer fuzz) when a C compiler
# is available.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fail() { echo "check failed: $*" >&2; exit 1; }

# Build profile and Store bundle.
grep -q -- '--cart-ram-kib 128' "$root/build.sh" || fail "128 KiB cartridge RAM profile"
grep -q -- '--multiplayer' "$root/build.sh" || fail "multiplayer enabled in the build"
grep -q 'pack-bundle' "$root/build.sh" || fail "Store bundle step"
grep -q '"cartridge_ram_kib":128' "$root/metadata.json" || fail "metadata RAM profile"

# Portable ABI: no floating point, no heap, no initialized pointer tables.
! grep -nE '\b(float|double|malloc|free)\b' "$root/game.c" || fail "float or heap in game.c"
! grep -nE 'static const char \*[a-zA-Z_]+\[' "$root/game.c" || fail "initialized pointer table"
grep -q 'skipass_madness_init' "$root/game.c" || fail "init export"
grep -q 'skipass_madness_update' "$root/game.c" || fail "update export"
grep -q 'skipass_madness_draw' "$root/game.c" || fail "draw export"

# Indexed colour for sprites and background.
grep -q 'prg32_sprite_draw_indexed' "$root/game.c" || fail "indexed sprites"
grep -q 'prg32_gfx_rect_indexed' "$root/game.c" || fail "indexed background"
grep -q 'prg32_palette_set' "$root/game.c" || fail "background palette"
! grep -q 'prg32_gfx_rect(' "$root/game.c" || fail "direct-colour rectangles"
grep -q 'spr_24x32' "$root/sprites.h" || fail "generated skier sprites"

# Pistes traced from the Alto Sangro map: tables generated, never hand-edited.
for piste in AREMOGNA 'MONTE PRATELLO' 'TOPPE-AREMOGNA' 'TOPPE DEL TESORO' PIZZALTO 'CRETE ROSSE'; do
  grep -q "$piste" "$root/pistes.h" || fail "piste $piste"
done
grep -q '#include "pistes.h"' "$root/game.c" || fail "game uses the traced pistes"
grep -q 'static void wall_band' "$root/game.c" || fail "gully walls"
tmp_p=$(mktemp)
cp "$root/pistes.h" "$tmp_p"
python3 "$root/scripts/trace_pistes.py" --out "$tmp_p" >/dev/null
cmp -s "$tmp_p" "$root/pistes.h" || { rm -f "$tmp_p"; fail "pistes.h is out of date with scripts/trace_pistes.py"; }
rm -f "$tmp_p"

# Slope physics.
grep -q 'static void physics_step' "$root/game.c" || fail "physics step"
grep -q 'Coulomb' "$root/game.c" || fail "snow friction"

# Music only on title/attract, win and game over; effects while skiing.
grep -q 'static void ski_audio' "$root/game.c" || fail "skiing effects"
grep -q 'static void audio_quiet' "$root/game.c" || fail "music stop in play"
[ "$(grep -c 'prg32_audio_play_track' "$root/game.c")" -eq 1 ] || fail "one music entry point"
python3 - "$root/assets/audio.json" <<'EOF' || fail "audio.json"
import json, sys
a = json.load(open(sys.argv[1]))
assert len(a["tracks"]) == 3, "title, win and game-over tracks"
pans = {i.get("default_pan", 0) for i in a["instruments"][:8]}
assert min(pans) < 0 < max(pans), "stereo music"
assert not a.get("samples"), "SID-like synthesis only"
EOF

# Multiplayer.
grep -q 'prg32_multiplayer_join' "$root/game.c" || fail "multiplayer join"
grep -q 'prg32_multiplayer_get_peer' "$root/game.c" || fail "peer rendering"

# Host harness.
if command -v cc >/dev/null 2>&1; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  san="-fsanitize=address,undefined"
  cc -std=c11 -O1 -g -Wall -Wextra -Werror $san -I"$root/tests/host" -o "$tmp/host" "$root/tests/host/host.c" 2>/dev/null ||
    cc -std=c11 -O1 -Wall -Wextra -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I"$root/tests/host" -o "$tmp/host" "$root/tests/host/host.c"
  "$tmp/host" sim | tee "$tmp/sim.txt"
  [ "$(wc -l < "$tmp/sim.txt")" -eq 6 ] || fail "autopilot did not finish all pistes"
  "$tmp/host" fuzz 120
fi
echo "source policy checks passed"
