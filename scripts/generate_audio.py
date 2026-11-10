#!/usr/bin/env python3
"""Generate the Skipass Madness AUDIO block source (assets/audio.json).

Everything here is original and deterministic. The block has no PCM: every
sound is a SID-like procedural oscillator, so the cartridge carries only
instrument descriptors and tracker events.

* Instruments 0-7 play the stereo tracker music. Instrument == channel.
  Pads are split hard left/right, the lead sits left of centre, the counter
  melody and arpeggio to the right, bass and kick in the centre.
* Instruments 8-15 are the in-game effects (ski hiss, wind, carving scrape,
  chime, thud, crash, pole click, whoosh); game.c plays them on channels 0-7
  only while the tracker is stopped.

Tracks (music only on these screens, never while skiing):
  0  title / attract  -- a saltarello-flavoured 6/8 dance (Abruzzo folk idiom)
  1  piste conquered  -- a bright 4/4 fanfare
  2  skipass expired  -- a slow minor lament with a wry ending
"""
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
TRI, SAW, PULSE, NOISE = 0, 1, 2, 3


def sid(wave, pulse, cutoff, res):
    return 0x8000 | (res << 10) | (cutoff << 6) | (pulse << 2) | wave


def inst(sample, vol, pan, a, d, s, r):
    return {"sample_id": sample, "default_volume": vol, "default_pan": pan,
            "attack": a, "decay": d, "sustain": s, "release": r}


INSTRUMENTS = [
    inst(sid(PULSE, 6, 12, 1), 52, -14, 2, 55, 150, 40),   # 0 lead (left of centre)
    inst(sid(TRI, 8, 15, 0), 40, 34, 4, 50, 170, 50),      # 1 counter melody (right)
    inst(sid(SAW, 8, 6, 1), 22, -52, 45, 80, 150, 80),     # 2 pad, hard left
    inst(sid(SAW, 8, 6, 1), 22, 52, 45, 80, 150, 80),      # 3 pad, hard right
    inst(sid(PULSE, 9, 6, 2), 56, 0, 1, 40, 120, 30),      # 4 bass
    inst(sid(PULSE, 3, 11, 0), 20, 40, 1, 30, 60, 25),     # 5 arpeggio (right)
    inst(sid(NOISE, 8, 13, 0), 34, -24, 0, 22, 0, 18),     # 6 tambourine / snare (left)
    inst(sid(TRI, 8, 9, 0), 62, 0, 0, 22, 0, 16),          # 7 kick drum
    # ---- effects, used only while skiing (tracker stopped)
    inst(sid(NOISE, 8, 10, 0), 255, 0, 12, 0, 255, 40),    # 8 ski-on-snow hiss (held)
    inst(sid(NOISE, 8, 3, 2), 255, 0, 60, 0, 255, 90),     # 9 wind (held, resonant)
    inst(sid(NOISE, 8, 14, 1), 255, 0, 2, 0, 255, 30),     # 10 carving / snowplough scrape (held)
    inst(sid(PULSE, 4, 15, 0), 255, 0, 0, 45, 0, 40),      # 11 checkpoint chime
    inst(sid(TRI, 8, 5, 1), 255, 0, 0, 40, 0, 30),         # 12 landing thud
    inst(sid(NOISE, 8, 9, 0), 255, 0, 0, 90, 0, 70),       # 13 crash
    inst(sid(PULSE, 2, 15, 0), 255, 0, 0, 10, 0, 10),      # 14 pole click / menu blip
    inst(sid(NOISE, 8, 7, 3), 255, 0, 8, 70, 0, 50),       # 15 whoosh (jump, near miss)
]

MUSIC_GAIN = 3.0   # headroom against clipping in captured QEMU PCM

NOTE = {n: i for i, n in enumerate(["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"])}


def midi(name):
    return 12 * (int(name[-1]) + 1) + NOTE[name[:-1]]


def build_track(tempo, bar_ticks, bars, melody, counter, style):
    ev = []

    def add(tick, cmd, a=None, b=None, order=1):
        ev.append((tick, order, cmd, a, b))

    def play(ch, start, length, note):
        add(start, "NOTE_ON", ch, note)
        add(start + max(1, length) - 1, "NOTE_OFF", ch, order=0)

    t = 0
    for chord, mel in zip(bars, melody):
        root, third, fifth = chord
        play(2, t, bar_ticks, third)
        play(3, t, bar_ticks, fifth)
        if style == "saltarello":
            play(4, t, 5, root - 24)
            play(4, t + 6, 5, fifth - 24)
            for step in range(6):
                play(5, t + step * 2, 1, (root, third, fifth, root + 12, fifth, third)[step] + 12)
            play(7, t, 2, 36)
            play(7, t + 6, 2, 36)
            for step in (2, 4, 8, 10):
                play(6, t + step, 1, 72)
            play(6, t + 3, 2, 60)
            play(6, t + 9, 2, 60)
        elif style == "fanfare":
            for step, note in enumerate((root - 24, root - 12, root - 24, fifth - 24)):
                play(4, t + step * 4, 3, note)
            play(7, t, 2, 36)
            play(7, t + 8, 2, 36)
            play(6, t + 4, 2, 60)
            play(6, t + 12, 2, 60)
            play(6, t + 14, 1, 64)
        else:  # lament: no drums, slow bass
            play(4, t, 7, root - 24)
            play(4, t + 8, 7, fifth - 24)
            for step in range(4):
                play(5, t + step * 4, 2, (root, third, fifth, third)[step] + 12)
        pos = t
        for name, length in mel:
            if name != "-":
                play(0, pos, length, midi(name))
            pos += length
        assert pos - t == bar_ticks, (pos - t, mel)
        t += bar_ticks
    pos = 0
    for name, length in counter:
        if name != "-":
            play(1, pos, length, midi(name))
        pos += length
    assert pos == t, (pos, t)
    ev.sort(key=lambda e: (e[0], e[1]))
    out = [{"delta": 0, "command": "SET_TEMPO", "arg0": tempo}]
    # The mixer applies the channel volume twice (voice gain x channel gain),
    # so set v = 255*sqrt(gain) per channel. game.c restores 255 on stop.
    for ch in range(8):
        gain = min(1.0, MUSIC_GAIN * INSTRUMENTS[ch]["default_volume"] / 255)
        out.append({"delta": 0, "command": "SET_VOLUME", "arg0": ch, "arg1": round(255 * gain ** 0.5)})
    last = 0
    for tick, _, cmd, a, b in ev:
        out[-1]["delta"] += tick - last
        last = tick
        x = {"delta": 0, "command": cmd, "arg0": a}
        if b is not None:
            x["arg1"] = b
        out.append(x)
    out[-1]["delta"] += t - last
    for ch in range(8):
        out.append({"delta": 0, "command": "NOTE_OFF", "arg0": ch})
    out[-1]["delta"] = 4
    out.append({"delta": 0, "command": "JUMP", "arg0": 0, "arg1": 0})
    return {"events": out}


C, Am, F, G, E, Dm = (60, 64, 67), (57, 60, 64), (53, 57, 60), (55, 59, 62), (52, 56, 59), (50, 53, 57)


def run(*notes):
    """Running eighth notes (2 ticks each) unless a duration is given."""
    out = []
    for n in notes:
        out.append(n if isinstance(n, tuple) else (n, 2))
    return out


# -- track 0: title / attract, 6/8 saltarello feel (12 ticks per bar)
SALT_BARS = [Am, Am, G, G, F, G, Am, Am, C, G, Am, E, F, G, Am, Am]
SALT_MELODY = [
    run("A4", "C5", "E5", "A5", "G5", "E5"),
    run("F5", "E5", "D5", ("E5", 4), "C5"),
    run("D5", "B4", "G4", "B4", "D5", "G5"),
    run("F5", "E5", "D5", ("B4", 6)),
    run("C5", "F5", "A5", "G5", "F5", "E5"),
    run("D5", "G5", "B5", "A5", "G5", "F5"),
    run("E5", "A5", "C6", "B5", "A5", "G#5"),
    run(("A5", 8), ("-", 4)),
    run("G5", "E5", "C5", "E5", "G5", "C6"),
    run("B5", "G5", "D5", ("G5", 4), "F5"),
    run("E5", "C5", "A4", "C5", "E5", "A5"),
    run(("G#5", 6), "B5", "G#5", "E5"),
    run("A5", "F5", "C5", "F5", "A5", "C6"),
    run("B5", "A5", "G5", "F5", "E5", "D5"),
    run("C5", "E5", "A5", "E5", "G#5", "B5"),
    run(("A5", 6), "E5", ("A4", 4)),
]
SALT_COUNTER = [("E4", 6), ("C4", 6), ("A3", 6), ("E4", 6), ("D4", 6), ("B3", 6), ("G3", 6), ("D4", 6),
                ("C4", 6), ("A3", 6), ("B3", 6), ("D4", 6), ("C4", 6), ("E4", 6), ("A3", 12),
                ("E4", 6), ("G4", 6), ("D4", 6), ("B3", 6), ("C4", 6), ("E4", 6), ("B3", 6), ("G#3", 6),
                ("A3", 6), ("C4", 6), ("B3", 6), ("D4", 6), ("E4", 6), ("C4", 6), ("A3", 12)]

# -- track 1: piste conquered, 4/4 fanfare (16 ticks per bar)
WIN_BARS = [C, F, G, C, Am, F, G, C]
WIN_MELODY = [
    [("G4", 2), ("C5", 2), ("E5", 2), ("G5", 6), ("E5", 2), ("G5", 2)],
    [("A5", 4), ("F5", 2), ("A5", 2), ("C6", 8)],
    [("B5", 2), ("A5", 2), ("G5", 4), ("D5", 4), ("B4", 4)],
    [("C5", 2), ("E5", 2), ("G5", 2), ("C6", 6), ("-", 4)],
    [("C6", 4), ("A5", 4), ("E5", 4), ("A5", 4)],
    [("F5", 2), ("G5", 2), ("A5", 4), ("C6", 4), ("A5", 4)],
    [("G5", 2), ("A5", 2), ("B5", 2), ("D6", 6), ("B5", 4)],
    [("C6", 12), ("-", 4)],
]
WIN_COUNTER = [("E4", 16), ("C4", 16), ("D4", 16), ("E4", 16), ("C4", 16), ("A3", 16), ("B3", 16), ("C4", 16)]

# -- track 2: skipass expired, slow minor lament (16 ticks per bar)
LOSE_BARS = [Am, Dm, E, Am, F, Dm, E, Am]
LOSE_MELODY = [
    [("E5", 6), ("D5", 2), ("C5", 4), ("A4", 4)],
    [("F5", 6), ("E5", 2), ("D5", 8)],
    [("B4", 4), ("G#4", 4), ("E4", 8)],
    [("A4", 12), ("-", 4)],
    [("C5", 4), ("A4", 4), ("F4", 4), ("A4", 4)],
    [("D5", 4), ("F5", 4), ("A4", 8)],
    [("G#4", 4), ("B4", 4), ("D5", 2), ("C5", 2), ("B4", 4)],
    [("A4", 4), ("E4", 2), ("A3", 6), ("-", 4)],
]
LOSE_COUNTER = [("C4", 16), ("A3", 16), ("G#3", 16), ("A3", 16), ("A3", 16), ("F3", 16), ("E3", 16), ("E3", 16)]


def main():
    tracks = [build_track(140, 12, SALT_BARS, SALT_MELODY, SALT_COUNTER, "saltarello"),
              build_track(120, 16, WIN_BARS, WIN_MELODY, WIN_COUNTER, "fanfare"),
              build_track(84, 16, LOSE_BARS, LOSE_MELODY, LOSE_COUNTER, "lament")]
    doc = {"samples": [], "instruments": INSTRUMENTS, "tracks": tracks}
    out = ROOT / "assets" / "audio.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"{out.relative_to(ROOT)}: {len(INSTRUMENTS)} instruments, "
          f"{len(tracks)} tracks ({', '.join(str(len(t['events'])) for t in tracks)} events)")


if __name__ == "__main__":
    main()
