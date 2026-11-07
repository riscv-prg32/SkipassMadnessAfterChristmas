# Architecture

The cartridge is a freestanding RV32IMAC C program using only PRG32's portable ABI. It targets the standard 128 KiB cartridge RAM profile and uses no heap allocation or floating point.

All scenery is drawn through indexed rectangles. The large rear-view skier is a two-frame, 4-bit indexed sprite with an eight-colour high-contrast local palette. Its descriptor is assembled at runtime so the portable cartridge contains no load-address-dependent initialized pointers. Layered snow-capped ranges, lift towers, gondolas, piste texture, shaded conifers, perspective gates, rocks and moguls provide depth. A deterministic object ring moves hazards from the horizon toward the skier and recycles them after they pass below the playfield, keeping the full 75-second descent populated while bounding memory and frame cost.

Multiplayer publishes course, position, distance and screen state through `prg32-multiplayer-1.0`; peers on the same course are rendered as rivals. Offline play is complete when networking is unavailable.

The AUDIO block contains SID-like procedural oscillators. Tracker tracks are used only on title/attract, win and game-over states. The skiing state stops the tracker and emits regular stereo-panned ski-surface noise plus distinct cues for carving, gates, moguls and impacts.
