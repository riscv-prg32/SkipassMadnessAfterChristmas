# Architecture

The cartridge is a freestanding RV32IMAC C program using only PRG32's portable ABI. It targets the standard 128 KiB cartridge RAM profile and uses no heap allocation or floating point.

All scenery is drawn through indexed rectangles. The skier is a two-frame, 4-bit indexed sprite with an eight-colour local palette. A deterministic object ring recycles trees, rocks and slalom gates after they pass above the piste horizon, keeping the full 75-second descent populated while bounding memory and frame cost.

Multiplayer publishes course, position, distance and screen state through `prg32-multiplayer-1.0`; peers on the same course are rendered as rivals. Offline play is complete when networking is unavailable.

The AUDIO block contains SID-like procedural oscillators. Tracker tracks are used only on title/attract, win and game-over states. The skiing state stops the tracker and emits short, stereo-panned procedural effects for carving, gates and impacts.
