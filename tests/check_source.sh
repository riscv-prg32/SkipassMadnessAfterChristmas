#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
grep -q -- '--cart-ram-kib 128' "$root/build.sh"
grep -q -- '--multiplayer' "$root/build.sh"
grep -q 'prg32_sprite_draw_indexed' "$root/game.c"
grep -q 'music_stop();' "$root/game.c"
grep -q 'prg32_multiplayer_join' "$root/game.c"
grep -q 'if(sy>205)' "$root/game.c"
grep -q 'result("PISTE CONQUERED!",course_name\[g.course\])' "$root/game.c"
grep -q 'static void bump' "$root/game.c"
grep -q 'static void ski_sound' "$root/game.c"
grep -q '#define SKIER_W 16' "$root/game.c"
grep -q 'skier.pixels=skier_px' "$root/game.c"
grep -q '"sample_id":34650' "$root/audio.json"
! grep -q '"synth"' "$root/audio.json"
echo "source policy checks passed"
