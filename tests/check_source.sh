#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
grep -q -- '--cart-ram-kib 128' "$root/build.sh"
grep -q -- '--multiplayer' "$root/build.sh"
grep -q 'prg32_sprite_draw_indexed' "$root/game.c"
grep -q 'music_stop();' "$root/game.c"
grep -q 'prg32_multiplayer_join' "$root/game.c"
echo "source policy checks passed"
