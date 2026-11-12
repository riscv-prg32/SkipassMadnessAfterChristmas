# The pistes, traced from the map

The six courses come from the Skipass Alto Sangro panoramic piste map (<https://www.datocms-assets.com/57366/1660222120-roccaraso-piste.pdf>, one 1335×629 image). `scripts/trace_pistes.py` turns hand traces of that map into `pistes.h`. The map image itself is not in this repository.

## From a perspective panorama to metres

The map is a painted perspective view, not a plan. Far ridges at the top are compressed much more than the valley floor in front, and the mountains are lifted by their altitude. One metres-per-pixel scale would stretch the valley and squash the summits. The tracer therefore models the panorama as a pinhole camera:

- principal point (668, 315) px;
- height 5500 m above a 1300 m base plane;
- pitch 60° down;
- focal length 700 px.

It back-projects every traced point onto the ground using that point's altitude. The camera was fitted (least squares on log distance) to four distances between labelled stations, so the parameters are estimates:

| From | To | Distance used |
|---|---|---|
| Piazzale Aremogna | Rivisondoli | ~6 km |
| Toppe del Tesoro | Piazzale Aremogna | ~2.2 km |
| Monte Pratello | Rivisondoli | ~3.8 km |
| Pizzalto | Piazzale Pizzalto | ~1.8 km |

With this model a pixel covers about 7.5 m at the Rivisondoli front and up to 13.6 m down-image at the summit ridge.

Turns are measured in that ground plan, so a bend that looks sharp because the map foreshortens it is not exaggerated.

## What each trace carries

Each piste is a polyline of map pixels. Its vertices carry:

- **Altitude anchors:** the map's labels (Toppe del Tesoro 2141, Monte Pratello 2044, Pizzalto 1850, P. Aremogna 1668, P. Pizzalto 1484, Rivisondoli 1309). In between are estimates read from the drawing, such as Valle Verde about 1850 and the Monte Pratello hut about 1440.
- **Half-width:** wide boulevards and narrow tracks, as drawn.
- **Gully walls**, per side:

  | Level | Meaning | Height |
  |---|---|---|
  | 1 | bank | 4 m |
  | 2 | gully side | 9 m |
  | 3 | canyon wall | 16 m |

  They are set where the map draws a dashed track or a channel squeezed between forest or rock: the D1-G1 chute, the C4-D4 track and the gap below it, the K2 forest corridor and the P1 valley.

With `--map`, the script also samples the image along the line:

- **forest density** on each side, from dark-green pixels 5–21 px out;
- **lift crossings**, from the pale yellow lift lines.

The splined line is resampled every 10 m into drop, turn, half-width and environment. Run `--overlay` to draw the traces back onto the map for checking.

## Game scale

At full scale the ridge traverses take minutes of pole-pushing. The course therefore keeps the map's shape and real altitudes, but its length is multiplied by `GAME_SCALE = 0.62`, which steepens the grades. The script prints both lengths:

| Piste | Map length | In game | Average grade | Steepest | Turning | Gully | Lifts |
|---|---|---|---|---|---|---|---|
| E1 Aremogna | 1.9 km | 1.2 km | 15% | 17% | 13° | — | 1 |
| M1 Monte Pratello | 4.1 km | 2.5 km | 12% | 30% | 239° | — | 0 |
| D4 Toppe-Aremogna | 3.9 km | 2.4 km | 20% | 53% | 312° | 640 m | 4 |
| D1 Toppe del Tesoro | 1.9 km | 1.2 km | 25% | 52% | 179° | 320 m | 2 |
| T8 Pizzalto | 2.6 km | 1.6 km | 23% | 35% | 162° | — | 4 |
| K2 Crete Rosse | 3.9 km | 2.4 km | 22% | 35% | 278° | 750 m | 4 |

"Turning" is the total change of direction. "Gully" counts the metres with walls on both sides. In-game plans and profiles are in [piste-plans.png](piste-plans.png), from `tests/host/host plan`.

## Editing a piste

1. Edit its vertices in `scripts/trace_pistes.py`.
2. Regenerate with the map, so the forest and lift sampling follow the new line:

   ```sh
   python3 scripts/trace_pistes.py --map <map.jpg> --overlay /tmp/overlay.png
   ```

3. Check the overlay.
4. Run `sh tests/check_source.sh`. It fails if `pistes.h` no longer matches the script.

Without `--map` the script keeps the sampled forest and lift data in `pistes.h`, but only for pistes whose sample count did not change.
