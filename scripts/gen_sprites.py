#!/usr/bin/env python3
"""Generate the 4-bit indexed sprite sheet used by Skipass Madness.

All sprites are original and drawn procedurally here from a small rear-view
skeleton (the camera sits behind and above the skier).  The script writes
``sprites.h`` with packed 4 bpp pixel data in three levels of detail:

* 24x32 (near), 12x16 (middle distance) and 6x8 (far), all sharing the same
  palette layout, so the cartridge only swaps the palette per character.

Palette layout (indices into the per-character palette built in game.c):

  0 transparent   1 outline       2 jacket        3 jacket shade
  4 trousers      5 trousers lit  6 helmet        7 goggles strap
  8 skis/board    9 poles        10 gloves/boots 11 white trim
 12 helmet shade 13 snow puff

Run ``python3 scripts/gen_sprites.py --preview out.png`` to inspect the
frames enlarged.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
W, H = 24, 32

T, OUT, JK, JKS, TR, TRL, HL, GS, SKI, POLE, DARK, WHITE, HLS, PUFF = range(14)

# Frame order is the ABI between this script and game.c (SPR_* constants).
FRAMES = [
    ("straight", dict(lean=0)),
    ("lean_l1", dict(lean=-1)),
    ("lean_l2", dict(lean=-2)),
    ("lean_r1", dict(lean=1)),
    ("lean_r2", dict(lean=2)),
    ("tuck", dict(lean=0, tuck=True)),
    ("tuck_l", dict(lean=-1, tuck=True)),
    ("tuck_r", dict(lean=1, tuck=True)),
    ("plough", dict(lean=0, plough=True)),
    ("air", dict(lean=0, air=True)),
    ("fallen", dict(fallen=True)),
    ("push", dict(lean=0, push=True)),
    ("boarder", dict(boarder=True)),
]


def capsule(d: ImageDraw.ImageDraw, a, b, width, color):
    d.line([a, b], fill=color, width=width)
    r = (width - 1) / 2
    for (x, y) in (a, b):
        if width > 2:
            d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def skier(lean=0, tuck=False, plough=False, air=False, fallen=False, push=False, boarder=False):
    img = Image.new("P", (W, H), T)
    d = ImageDraw.Draw(img)
    poles: list[tuple] = []
    if fallen:
        # Sprawled on the snow: crossed skis, body lying across, snow puff.
        d.ellipse([1, 22, 22, 31], fill=PUFF)
        d.polygon([(2, 27), (21, 22), (22, 24), (3, 29)], fill=SKI)
        d.polygon([(4, 21), (20, 28), (19, 30), (3, 23)], fill=SKI)
        d.polygon([(5, 19), (15, 17), (17, 23), (6, 25)], fill=JK)
        d.polygon([(6, 23), (16, 21), (17, 23), (7, 25)], fill=JKS)
        capsule(d, (15, 21), (21, 25), 3, TR)
        capsule(d, (14, 19), (20, 18), 3, TR)
        d.ellipse([1, 17, 7, 23], fill=HL)
        d.line([(1, 20), (7, 20)], fill=GS)
        poles = [((9, 16), (2, 13)), ((15, 25), (22, 30))]
        return finish(img, poles)
    if boarder:
        # A snowboarder sitting on the piste, board across the slope.
        d.polygon([(1, 28), (23, 28), (23, 31), (1, 31)], fill=SKI)
        capsule(d, (7, 27), (10, 21), 4, TR)
        capsule(d, (17, 27), (14, 21), 4, TR)
        d.polygon([(7, 13), (17, 13), (18, 24), (6, 24)], fill=JK)
        d.polygon([(7, 20), (17, 20), (18, 24), (6, 24)], fill=JKS)
        capsule(d, (7, 15), (4, 23), 3, JK)
        capsule(d, (17, 15), (20, 23), 3, JK)
        d.ellipse([8, 5, 16, 13], fill=HL)
        d.line([(8, 9), (16, 9)], fill=GS)
        d.point([(4, 24), (20, 24)], fill=DARK)
        return finish(img, [])

    cx = 12
    ground = 31
    knee_drop = 0
    if tuck:
        knee_drop = 6
    if air:
        ground = 28
    # Ski stance: skis go to the outside of the turn, body to the inside.
    foot_shift = -lean * 1.3
    body_shift = lean * 1.4
    head_shift = lean * 3.0
    gap = 3 if not plough else 5
    lx, rx = cx - gap + foot_shift, cx + gap + foot_shift
    tilt = lean * 0.8
    # Skis: tails toward the camera, tips running up into the distance.
    for fx, side in ((lx, -1), (rx, 1)):
        if plough:
            tail, tip = fx + side * 3, fx - side * 1
        else:
            tail, tip = fx + tilt, fx + tilt * 0.5
        if air:
            d.polygon([(tail - 2, ground), (tail + 2, ground), (tip + 1, ground - 6), (tip - 1, ground - 6)], fill=SKI)
        else:
            d.polygon([(tail - 1.5, ground), (tail + 1.5, ground), (tip + 1, ground - 10), (tip - 1, ground - 10)], fill=SKI)
    boot_y = ground - 6
    hip_y = 17 + knee_drop
    hip_x = cx + body_shift
    sh_y = (9 if not tuck else 17) + (1 if air else 0)
    sh_x = cx + head_shift * 0.75
    # Legs (boot -> knee -> hip); knees press inward in the snowplough.
    for fx, side in ((lx, -1), (rx, 1)):
        kx = fx + (side * -1.5 if plough else lean * 0.6) + (side * 1 if tuck else 0)
        ky = boot_y - 5 + (2 if tuck else 0) - (1 if air else 0)
        capsule(d, (fx, boot_y), (kx, ky), 4, TR)
        capsule(d, (kx, ky), (hip_x + side * 2.5, hip_y), 5, TR)
        d.line([(kx + side, ky - 1), (kx + side, ky + 2)], fill=TRL)
        d.rectangle([fx - 2, boot_y - 1, fx + 1, boot_y + 1], fill=DARK)
    # Torso.
    if tuck:
        d.polygon([(hip_x - 6, hip_y - 1), (hip_x + 6, hip_y - 1), (sh_x + 7, sh_y - 5), (sh_x - 7, sh_y - 5)], fill=JK)
        d.polygon([(hip_x - 6, hip_y - 1), (hip_x + 6, hip_y - 1), (hip_x + 6, hip_y - 3), (hip_x - 6, hip_y - 3)], fill=JKS)
        d.line([(sh_x - 5, sh_y - 3), (sh_x + 5, sh_y - 3)], fill=WHITE)
    else:
        d.polygon([(hip_x - 4, hip_y), (hip_x + 4, hip_y), (sh_x + 5, sh_y + 1), (sh_x - 5, sh_y + 1)], fill=JK)
        d.polygon([(hip_x - 4, hip_y), (hip_x + 4, hip_y), (hip_x + 4, hip_y - 2), (hip_x - 4, hip_y - 2)], fill=JKS)
        d.line([(sh_x - 4, sh_y + 4), (sh_x + 4, sh_y + 4)], fill=WHITE)
        d.line([(hip_x, hip_y - 4), (sh_x, sh_y + 2)], fill=JKS)
    # Head: helmet with goggles strap, seen from behind.
    hx = cx + head_shift
    hy = sh_y - 7 if not tuck else sh_y - 9
    hy += 1
    d.ellipse([hx - 3, hy, hx + 3, hy + 6], fill=HL)
    d.line([(hx - 2, hy + 5), (hx + 2, hy + 5)], fill=HLS)
    d.line([(hx - 3, hy + 3), (hx + 3, hy + 3)], fill=GS)
    # Arms and poles.
    for side in (-1, 1):
        sx = sh_x + side * 4
        if tuck:
            hand = (sx + side * 1, sh_y + 1)
            capsule(d, (sx, sh_y - 3), hand, 3, JK)
            poles.append((hand, (hand[0] + side * 5, hand[1] - 5)))
        elif push:
            hand = (sx + side * 3, sh_y + 9)
            capsule(d, (sx, sh_y + 2), hand, 3, JK)
            poles.append((hand, (hand[0] + side * 1, ground)))
        elif air:
            hand = (sx + side * 5, sh_y + 3)
            capsule(d, (sx, sh_y + 2), hand, 3, JK)
            poles.append((hand, (hand[0] + side * 3, hand[1] + 9)))
        else:
            reach = 2 + (1 if side * lean > 0 else 0)
            hand = (sx + side * reach, sh_y + 8)
            capsule(d, (sx, sh_y + 2), hand, 3, JK)
            poles.append((hand, (hand[0] + side * 3, ground - 2 - (2 if side * lean > 0 else 0))))
    return finish(img, poles)


def finish(img: Image.Image, poles) -> Image.Image:
    px = img.load()
    body = [[px[x, y] for x in range(W)] for y in range(H)]
    for y in range(H):
        for x in range(W):
            if body[y][x] != T:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and body[ny][nx] not in (T, PUFF, OUT):
                    px[x, y] = OUT
                    break
    d = ImageDraw.Draw(img)
    for a, b in poles:
        d.line([a, b], fill=POLE)
        d.point([b], fill=DARK)
        d.point([a], fill=DARK)
    return img


def downsample(img: Image.Image, w: int, h: int) -> Image.Image:
    """Majority-vote downsampling keeps outlines and small colour accents."""
    src = img.load()
    out = Image.new("P", (w, h), T)
    dst = out.load()
    fx, fy = W // w, H // h
    for y in range(h):
        for x in range(w):
            counts: dict[int, int] = {}
            for yy in range(fy):
                for xx in range(fx):
                    c = src[x * fx + xx, y * fy + yy]
                    counts[c] = counts.get(c, 0) + (1 if c in (T, OUT) else 2)
            best = max(counts.items(), key=lambda kv: kv[1])[0]
            dst[x, y] = best
    return out


def pack(img: Image.Image) -> list[int]:
    px = img.load()
    w, h = img.size
    data = []
    for y in range(h):
        for x in range(0, w, 2):
            data.append((px[x, y] << 4) | px[x + 1, y])
    return data


PREVIEW_PAL = [
    (0, 120, 140), (0, 0, 0), (200, 20, 30), (120, 0, 10), (20, 30, 90), (60, 80, 180), (240, 240, 240),
    (255, 160, 0), (255, 220, 0), (160, 160, 160), (40, 40, 40), (255, 255, 255), (170, 170, 190),
    (230, 240, 255),
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "sprites.h")
    ap.add_argument("--preview", type=Path)
    args = ap.parse_args()
    frames = [skier(**kw) for _, kw in FRAMES]
    lods = [(24, 32, frames), (12, 16, [downsample(f, 12, 16) for f in frames]),
            (6, 8, [downsample(f, 6, 8) for f in frames])]
    lines = ["/* Generated by scripts/gen_sprites.py -- do not edit. */",
             "#ifndef SKIPASS_SPRITES_H", "#define SKIPASS_SPRITES_H", "#include <stdint.h>",
             f"#define SPR_FRAMES {len(frames)}"]
    for i, (name, _) in enumerate(FRAMES):
        lines.append(f"#define SPR_{name.upper()} {i}")
    for w, h, imgs in lods:
        data = [b for im in imgs for b in pack(im)]
        lines.append(f"static const uint8_t spr_{w}x{h}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("  " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
    lines.append("#endif")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.out} ({len(frames)} frames x 3 LODs)")
    if args.preview:
        scale = 6
        sheet = Image.new("RGB", (len(frames) * (W + 2) * scale, (H + 16 + 8 + 6) * scale), (0, 90, 110))
        flat = [c for rgb in PREVIEW_PAL for c in rgb] + [0] * (768 - 3 * len(PREVIEW_PAL))
        y = 0
        for w, h, imgs in lods:
            for i, im in enumerate(imgs):
                im = im.copy()
                im.putpalette(flat)
                sheet.paste(im.convert("RGB").resize((w * scale, h * scale), Image.NEAREST),
                            (i * (W + 2) * scale, y * scale))
            y += h + 2
        sheet.save(args.preview)


if __name__ == "__main__":
    main()
