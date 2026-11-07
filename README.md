# Skipass Madness After Christmas

An original 320×200 PRG32 downhill skiing cartridge inspired by the play rhythm of *Mountain Madness – Super Pro Skiing* and by the real Roccaraso–Rivisondoli ski area in Abruzzo, Italy.

The six selectable routes use the real piste-area names Aremogna, Pallottieri, Gravare, Pizzalto, Monte Pratello and Crete Rosse. The geometry, hazards and pixel scenery are an arcade interpretation, not a navigation map.

## Controls

- Left/Right: carve
- Up/Down: accelerate/brake
- A: sharp carve
- Start: begin or return to the title

Network consoles in the same PRG32 multiplayer room appear as orange rivals. Music is limited to title/attract, win and game-over screens; downhill play uses stereo ski-surface, carving, mogul, collision and gate effects. The scene uses a high-contrast rear-view skier, perspective gates and moguls, layered snow-capped mountains, lift cables, gondolas and snow-covered conifers.

## Build

Requires a sibling checkout of `riscv-prg32/PRG32` (or `PRG32_REPO`):

```sh
./build.sh
```

This produces portable 128 KiB-profile cartridges for ESP32-C6 and QEMU in `dist/`.

## References

- PRG32: https://github.com/riscv-prg32/PRG32
- Roccaraso piste map: https://www.datocms-assets.com/57366/1660222120-roccaraso-piste.pdf
- Gameplay reference: https://www.youtube.com/watch?v=_IjlVi3aDSU
