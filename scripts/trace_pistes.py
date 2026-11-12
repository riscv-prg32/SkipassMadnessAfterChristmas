#!/usr/bin/env python3
"""Build the six piste tables in pistes.h from traces of the Alto Sangro map.

The traces are polylines in pixel coordinates of the panoramic piste map
(https://www.datocms-assets.com/57366/1660222120-roccaraso-piste.pdf, embedded
image 1335x629). Each vertex may carry an altitude anchor (the map's labelled
peaks, huts and stations; intermediate ones estimated from the drawing), a
piste half-width and gully walls. The script

  * smooths each polyline (Catmull-Rom) and back-projects it onto the ground.
    The map is a perspective panorama, so there is no single scale: the
    script models it as a pinhole camera (height, pitch, focal length fitted
    to plausible distances between labelled stations, see CAMERA) and lifts
    every point by its altitude, so far ridges at the top of the image get
    more metres per pixel than the valley floor in front;
  * resamples it every 10 m: drop, change of direction, half-width, walls;
  * if the map image is given (--map), samples it along the line to find the
    lift lines the piste passes under (yellow) and how dense the forest is on
    each side (dark green), so the scenery follows the map;
  * writes pistes.h (const tables only: no pointers) and, with --overlay, a
    PNG of the traces over the map for checking.

The map image is not redistributed: without --map the script keeps the
sampled lift and forest data already present in pistes.h.
"""
from __future__ import annotations

import argparse
import math
import re
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
# Panorama camera: principal point (px), height above the 1300 m base (m),
# downward pitch (deg), focal length (px). Fitted by scripts/trace_pistes.py
# --calibrate to: Aremogna-Rivisondoli ~6 km, Toppe del Tesoro-Aremogna
# ~2.2 km, Monte Pratello-Rivisondoli ~3.8 km, Pizzalto-P. Pizzalto ~1.8 km.
CAMERA = dict(cx=668, cy=315, h=5500, pitch=60, f=700)
STEP = 10.0                # metres per course sample
# Game scale: the course keeps the map's shape and real altitudes, with its
# ground length shortened to this fraction so runs last minutes, not tens
# of minutes of pole-pushing along the ridges. Grades steepen accordingly.
GAME_SCALE = 0.62

# Vertex: (x, y, altitude m or None, half-width m or None, wall_left, wall_right)
# walls 0..3: 0 open, 1 bank (~4 m), 2 gully side (~9 m), 3 canyon wall (~16 m).
# Huts: (vertex index, hut id, side) -> placed at that vertex.
PISTES = [
    dict(code="E1", name="AREMOGNA", finish="P. AREMOGNA", diff=0,
         note="Valle Verde to Piazzale Aremogna: the wide blue boulevard",
         pts=[(679, 211, 1850, 20, 0, 0), (662, 220, None, 22, 0, 0), (640, 231, None, 24, 0, 0),
              (612, 244, None, 22, 0, 0), (585, 256, 1760, 24, 0, 0), (556, 268, None, 26, 0, 0),
              (528, 279, None, 24, 0, 0), (500, 289, 1690, 28, 0, 0), (476, 297, 1668, 32, 0, 0)],
         huts=[(0, 4, 1), (8, 1, 0)]),
    dict(code="M1", name="MONTE PRATELLO", finish="PRATO", diff=0,
         note="Along the Monte Pratello ridge, round the hairpin above Prato",
         pts=[(905, 160, 2040, 16, 1, 0), (930, 168, None, 18, 1, 0), (965, 171, None, 18, 1, 0),
              (1000, 172, 1985, 18, 1, 0), (1040, 170, None, 18, 1, 0), (1075, 168, 1950, 18, 0, 0),
              (1110, 162, None, 16, 0, 0), (1150, 163, 1905, 18, 0, 0), (1185, 175, None, 18, 0, 0),
              (1210, 195, 1860, 20, 0, 0), (1226, 218, None, 20, 0, 0), (1228, 240, 1815, 20, 0, 0),
              (1220, 262, None, 22, 0, 0), (1212, 280, 1775, 24, 0, 0), (1215, 298, 1745, 30, 0, 0)],
         huts=[(0, 6, 1), (14, 7, 1)]),
    dict(code="D4", name="TOPPE-AREMOGNA", finish="P. AREMOGNA", diff=1,
         note="Summit ridge west, the hairpin, the C4-D4 forest track and the gap to Aremogna",
         pts=[(570, 85, 2141, 14, 0, 0), (540, 87, None, 14, 0, 0), (500, 96, 2105, 14, 0, 0),
              (460, 106, None, 14, 1, 0), (430, 116, 2060, 14, 1, 0), (410, 128, None, 14, 1, 0),
              (402, 145, 2010, 16, 0, 0), (399, 165, None, 16, 0, 0), (403, 183, 1945, 14, 0, 1),
              (418, 201, None, 9, 2, 2), (445, 218, 1895, 9, 2, 2), (475, 229, None, 10, 1, 2),
              (503, 238, 1845, 12, 1, 1), (504, 250, None, 9, 3, 3), (497, 264, 1775, 9, 3, 3),
              (484, 280, 1720, 12, 2, 2), (468, 296, 1668, 26, 0, 0)],
         huts=[(0, 3, 1), (16, 1, 0)]),
    dict(code="D1", name="TOPPE DEL TESORO", finish="VALLE VERDE", diff=2,
         note="The black: summit ridge east, the drop to G2 and the D1-G1 chute",
         pts=[(572, 84, 2141, 12, 0, 0), (598, 85, None, 12, 0, 0), (622, 92, 2110, 12, 0, 0),
              (638, 104, None, 12, 1, 0), (650, 118, 2085, 12, 1, 0), (664, 130, None, 12, 0, 0),
              (676, 143, 2035, 11, 0, 0), (677, 156, None, 8, 2, 2), (674, 170, 1965, 7, 3, 3),
              (673, 184, None, 7, 3, 3), (676, 197, 1885, 8, 2, 2), (679, 210, 1850, 18, 0, 0)],
         huts=[(0, 3, 0), (11, 4, 1)]),
    dict(code="T8", name="PIZZALTO", finish="P. PIZZALTO", diff=1,
         note="Under the Pizzalto summit, the long diagonal face, round onto T2",
         pts=[(648, 310, 1850, 14, 0, 0), (626, 314, None, 14, 0, 0), (604, 319, 1835, 14, 0, 0),
              (585, 330, None, 16, 0, 0), (566, 352, 1790, 20, 0, 0), (548, 378, None, 22, 0, 0),
              (531, 405, 1700, 22, 0, 0), (516, 432, None, 22, 0, 0), (505, 458, 1610, 20, 0, 0),
              (500, 478, None, 18, 0, 0), (505, 494, 1550, 18, 0, 0), (528, 507, None, 18, 0, 0),
              (556, 517, 1505, 20, 0, 0), (583, 527, 1484, 28, 0, 0)],
         huts=[(13, 11, 0)]),
    dict(code="K2", name="CRETE ROSSE", finish="RIVISONDOLI", diff=1,
         note="From Crete Rosse through the forest S-bends to hut 9, then down the P1 valley",
         pts=[(746, 284, 1830, 12, 0, 0), (758, 300, None, 13, 0, 0), (785, 316, 1790, 13, 1, 1),
              (810, 333, None, 13, 1, 1), (826, 352, 1715, 12, 2, 1), (838, 374, None, 12, 2, 2),
              (858, 390, 1630, 12, 1, 2), (888, 402, None, 13, 1, 1), (920, 413, 1560, 13, 1, 1),
              (955, 421, None, 13, 1, 1), (990, 430, 1475, 14, 1, 1), (1027, 440, 1440, 16, 0, 0),
              (1048, 458, None, 11, 2, 2), (1072, 478, 1405, 10, 3, 2), (1092, 503, None, 10, 3, 3),
              (1106, 528, 1350, 10, 2, 3), (1124, 551, None, 12, 2, 2), (1146, 566, 1309, 26, 0, 0)],
         huts=[(1, 5, 1), (11, 8, 1), (17, 9, 0)]),
]

HUTS = ["CHEZ NOUS", "TELECABINA", "PARADISO", "RIFUGIO", "VALLE VERDE", "CRETE ROSSE",
        "ESAGONO", "PRATO", "MONTE PRATELLO", "FONTANILE", "LAGO D'AVOLI", "PIZZALTO"]


def ground(x, y, alt, cam=CAMERA):
    """Back-project map pixel (x, y) at a known altitude onto the ground (m)."""
    yp = (cam["cy"] - y) / cam["f"]
    dh = (alt - 1300) - cam["h"]
    s, c = math.sin(math.radians(cam["pitch"])), math.cos(math.radians(cam["pitch"]))
    z = -dh * (yp * s + c) / (s - yp * c)
    fwd = z * c - dh * s
    return (x - cam["cx"]) * fwd / cam["f"], z


def catmull(pts, per=12):
    out = []
    n = len(pts)
    for i in range(n - 1):
        p0, p1, p2, p3 = pts[max(i - 1, 0)], pts[i], pts[i + 1], pts[min(i + 2, n - 1)]
        for k in range(per):
            t = k / per
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * (2 * p1[j] + (-p0[j] + p2[j]) * t + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3) for j in range(2)))
    out.append(pts[-1][:2])
    return out


def interp_anchor(dist, anchors):
    """Piecewise-linear in distance between (d, value) anchors."""
    if dist <= anchors[0][0]:
        return anchors[0][1]
    for (d0, a0), (d1, a1) in zip(anchors, anchors[1:]):
        if dist <= d1:
            return a0 + (a1 - a0) * (dist - d0) / max(d1 - d0, 1e-6)
    return anchors[-1][1]


def build(piste, img=None):
    pts = piste["pts"]
    fine = catmull(pts)
    # altitude of every fine point: anchors interpolated along the pixel path
    pd = [0.0]
    for a, b in zip(fine, fine[1:]):
        pd.append(pd[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    pv = [pd[min(i * 12, len(pd) - 1)] for i in range(len(pts))]
    pa = [(pv[i], p[2]) for i, p in enumerate(pts) if p[2] is not None]
    gnd = [ground(x, y, interp_anchor(d, pa)) for (x, y), d in zip(fine, pd)]
    # ground distance of every fine point (metres) and of every vertex
    fd = [0.0]
    for a, b in zip(gnd, gnd[1:]):
        fd.append(fd[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    real = fd[-1]
    fd = [d * GAME_SCALE for d in fd]
    vd = [fd[min(i * 12, len(fd) - 1)] for i in range(len(pts))]
    length = fd[-1]
    n = int(length // STEP)
    alt_a = [(vd[i], p[2]) for i, p in enumerate(pts) if p[2] is not None]
    hw_a = [(vd[i], p[3]) for i, p in enumerate(pts) if p[3] is not None]

    def at(dist):
        j = 0
        while j < len(fd) - 2 and fd[j + 1] < dist:
            j += 1
        t = (dist - fd[j]) / max(fd[j + 1] - fd[j], 1e-6)
        a, b = fine[j], fine[j + 1]
        ga, gb = gnd[j], gnd[j + 1]
        x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        # ground direction, measured like the image: x right, toward the viewer down
        ang = math.atan2(-(gb[1] - ga[1]), gb[0] - ga[0])
        pang = math.atan2(b[1] - a[1], b[0] - a[0])
        return x, y, ang, pang

    def wall(dist, side):
        # nearest-vertex walls, eased over 30 m
        best = None
        for i, p in enumerate(pts):
            w = p[4 + side]
            dd = abs(vd[i] - dist)
            if best is None or dd < best[0]:
                best = (dd, w)
        return best[1]

    rows = []
    prev_ang = None
    for k in range(n + 1):
        d = k * STEP
        x, y, ang, pang = at(min(d, length - 0.01))
        alt = interp_anchor(d, alt_a)
        alt2 = interp_anchor(d + STEP, alt_a)
        hw = interp_anchor(d, hw_a)
        bend = 0.0 if prev_ang is None else math.degrees(math.atan2(math.sin(ang - prev_ang), math.cos(ang - prev_ang)))
        prev_ang = ang
        rows.append(dict(x=x, y=y, ang=math.degrees(ang), pang=pang, alt=alt, drop=max(alt - alt2, 0.0), hw=hw, bend=bend,
                         wl=wall(d, 0), wr=wall(d, 1), fl=0, fr=0, lift=0))
    # walls: a single-sample spike is noise; keep runs
    if img is not None:
        sample_map(rows, img)
    return dict(length=length, real=real, rows=rows, vd=vd)


def is_forest(c):
    r, g, b = c[:3]
    return g < 120 and r < 90 and b < 120 and g >= r


def is_lift(c):
    r, g, b = c[:3]   # the pale yellow lift lines, blurred into the snow by the JPEG
    return r > 150 and g > 140 and (r + g) / 2 - b > 25


def sample_map(rows, img):
    W, H = img.size
    px = img.load()
    for k, r in enumerate(rows):
        a = r["pang"]
        # perpendicular in pixel space (to the right of the direction of travel)
        nx, ny = -math.sin(a), math.cos(a)
        for side, key in ((-1, "fl"), (1, "fr")):
            hits = tot = 0
            for off in range(5, 22, 2):
                sx, sy = int(r["x"] + side * nx * off), int(r["y"] + side * ny * off)
                if 0 <= sx < W and 0 <= sy < H:
                    tot += 1
                    hits += is_forest(px[sx, sy])
            f = hits / tot if tot else 0
            r[key] = 0 if f < 0.15 else (1 if f < 0.4 else (2 if f < 0.7 else 3))
        x, y = int(r["x"]), int(r["y"])
        r["yel"] = any(is_lift(px[x + i, y + j]) for i in range(-2, 3) for j in range(-2, 3) if 0 <= x + i < W and 0 <= y + j < H)
    # a lift crossing is the middle of each run of yellow samples
    k = 0
    while k < len(rows):
        if rows[k].get("yel"):
            j = k
            while j + 1 < len(rows) and rows[j + 1].get("yel"):
                j += 1
            m = (k + j) // 2
            if 4 < m < len(rows) - 4 and not any(rows[q]["lift"] for q in range(max(m - 15, 0), m)):
                rows[m]["lift"] = 1
            k = j + 1
        else:
            k += 1


def old_env(text):
    """Keep sampled forest/lift data from an existing pistes.h."""
    env = {}
    for m in re.finditer(r"/\* (\w\d) env \*/\s*\{([^}]*)\}", text):
        env[m.group(1)] = [int(v) for v in m.group(2).replace("\n", " ").split(",") if v.strip()]
    lifts = {}
    for m in re.finditer(r"/\* (\w\d) lifts \*/\s*\{([^}]*)\}", text):
        n = int(re.search(m.group(1) + r"_NLIFTS (\d+)", text).group(1))
        lifts[m.group(1)] = [int(v) for v in m.group(2).split(",") if v.strip()][:n]
    return env, lifts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", type=Path, help="the map image extracted from the PDF (pdfimages -j)")
    ap.add_argument("--overlay", type=Path)
    ap.add_argument("--out", type=Path, default=HERE / "pistes.h")
    args = ap.parse_args()
    img = None
    if args.map:
        from PIL import Image
        img = Image.open(args.map).convert("RGB")
    prev_env, prev_lifts = old_env(args.out.read_text()) if args.out.exists() and img is None else ({}, {})
    built = [build(p, img) for p in PISTES]
    maxn = max(len(b["rows"]) for b in built)

    L = []
    L.append("/* Generated by scripts/trace_pistes.py from traces of the Alto Sangro piste")
    L.append(" * map. Do not edit by hand. Per 10 m sample: drop (dm), bend (1/8 deg,")
    L.append(" * right positive), half width (25 cm), env bits: forest left 0-1, forest")
    L.append(" * right 2-3, wall left 4-5, wall right 6-7. */")
    L.append("#ifndef SKIPASS_PISTES_H")
    L.append("#define SKIPASS_PISTES_H")
    L.append(f"#define PISTE_MAXN {maxn}")
    L.append(f"#define NPISTE {len(PISTES)}")
    L.append("static const uint16_t PISTE_N[NPISTE] = {" + ", ".join(str(len(b["rows"])) for b in built) + "};")
    L.append("static const int16_t TOP_M[NPISTE] = {" + ", ".join(str(round(b["rows"][0]["alt"])) for b in built) + "};")
    L.append("static const int16_t BOT_M[NPISTE] = {" + ", ".join(str(p["pts"][-1][2]) for p in PISTES) + "};")
    L.append("static const int16_t BEARING[NPISTE] = {" + ", ".join(str(round(b["rows"][1]["ang"]) % 360) for b in built) + "};   /* map direction at the start, degrees */")
    L.append("static const uint8_t DIFF[NPISTE] = {" + ", ".join(str(p["diff"]) for p in PISTES) + "};")
    L.append("static const char PCODE[NPISTE][3] = {" + ", ".join(f'"{p["code"]}"' for p in PISTES) + "};")
    L.append("static const char PNAME[NPISTE][17] = {" + ", ".join(f'"{p["name"]}"' for p in PISTES) + "};")
    L.append("static const char PFINISH[NPISTE][13] = {" + ", ".join(f'"{p["finish"]}"' for p in PISTES) + "};")
    L.append("static const char HUTN[12][15] = {" + ", ".join(f'"{h}"' for h in HUTS) + "};")
    for p, b in zip(PISTES, built):
        rows = b["rows"]
        code = p["code"]
        if img is None and code in prev_env and len(prev_env[code]) == len(rows):
            for r, e in zip(rows, prev_env[code]):
                r["fl"], r["fr"] = e & 3, (e >> 2) & 3
        if img is None and code in prev_lifts:
            for m in prev_lifts[code]:
                if m < len(rows):
                    rows[m]["lift"] = 1
        L.append(f"/* {code} {p['name']}: {p['note']}; {b['real']:.0f} m on the map, {b['length']:.0f} m in game, "
                 f"{rows[0]['alt']:.0f}-{p['pts'][-1][2]} m */")
        L.append(f"static const uint8_t {code}_DROP[{len(rows)}] = {{" + ",".join(str(min(255, round(r['drop'] * 10))) for r in rows) + "};")
        L.append(f"static const int8_t {code}_BEND[{len(rows)}] = {{" + ",".join(str(max(-127, min(127, round(r['bend'] * 8)))) for r in rows) + "};")
        L.append(f"static const uint8_t {code}_HW[{len(rows)}] = {{" + ",".join(str(min(255, round(r['hw'] * 4))) for r in rows) + "};")
        L.append(f"static const uint8_t {code}_ENV[{len(rows)}] = /* {code} env */ {{" +
                 ",".join(str(r['fl'] | r['fr'] << 2 | r['wl'] << 4 | r['wr'] << 6) for r in rows) + "};")
        lifts = [k for k, r in enumerate(rows) if r["lift"]]
        L.append(f"static const uint16_t {code}_LIFTS[{max(len(lifts), 1)}] = /* {code} lifts */ {{" + ",".join(str(v) for v in (lifts or [0])) + "};")
        L.append(f"#define {code}_NLIFTS {len(lifts)}")
        huts = [(round(b['vd'][i] / STEP), h, s) for i, h, s in p["huts"]]
        L.append(f"static const uint16_t {code}_HUTS[{len(huts)}][3] = {{" + ",".join(f"{{{min(k, len(rows) - 1)},{h},{s}}}" for k, h, s in huts) + "};")
    L.append("#endif")
    args.out.write_text("\n".join(L) + "\n")

    for p, b in zip(PISTES, built):
        rows = b["rows"]
        steep = max(r["drop"] for r in rows) / STEP * 100
        turn = sum(abs(r["bend"]) for r in rows)
        walls = sum(1 for r in rows if r["wl"] >= 2 and r["wr"] >= 2) * STEP
        print(f"{p['code']} {p['name']:17s} map {b['real']:5.0f} m game {b['length']:5.0f} m  {rows[0]['alt']:.0f}->{p['pts'][-1][2]} m  "
              f"avg {(rows[0]['alt'] - p['pts'][-1][2]) / b['length'] * 100:4.1f}%  max {steep:4.1f}%  "
              f"turning {turn:4.0f} deg  gully {walls:4.0f} m  lifts {sum(r['lift'] for r in rows)}")

    if args.overlay and img is not None:
        from PIL import ImageDraw
        ov = img.copy()
        d = ImageDraw.Draw(ov)
        colours = [(0, 200, 255), (0, 120, 255), (255, 60, 0), (0, 0, 0), (255, 0, 160), (160, 0, 255)]
        for b, c in zip(built, colours):
            xy = [(r["x"], r["y"]) for r in b["rows"]]
            d.line(xy, fill=c, width=2)
            for r in b["rows"]:
                if r["wl"] >= 2 or r["wr"] >= 2:
                    d.ellipse((r["x"] - 2, r["y"] - 2, r["x"] + 2, r["y"] + 2), outline=(255, 255, 0))
                if r["lift"]:
                    d.rectangle((r["x"] - 3, r["y"] - 3, r["x"] + 3, r["y"] + 3), outline=(255, 0, 0), width=2)
        ov.save(args.overlay)


if __name__ == "__main__":
    main()
