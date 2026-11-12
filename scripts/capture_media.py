#!/usr/bin/env python3
"""Capture real Store media for Skipass Madness from the PRG32 QEMU target.

The script boots a clean copy of the PRG32 QEMU flash with only the freshly
built QEMU cartridge in cart0, then:

* paces and records the emulator's real 22050 Hz mono UART PCM stream,
* records the QEMU SDL window with macOS ``screencapture`` (Espressif QEMU
  has no QMP ``screendump``) and crops the cartridge viewport by scale,
* injects documented UART keyboard bytes on an emulator-time schedule,
* crops the 320x200 cartridge viewport out of the firmware display and
  muxes an H.264/AAC MP4, an MP3 of the same captured PCM, a PNG
  screenshot and a 128x128 icon cropped from the same real frame.

Key events are scheduled on the PCM clock (samples received / 22050). Run it
after ``source $HOME/esp-idf/export.sh`` so the Espressif QEMU with SDL is used.
"""

from __future__ import annotations

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("PRG32_REPO", HERE.parent / "PRG32")).resolve()
RATE = 22_050
AUDIO_PORT = 4421
CONSOLE_PORT = 4422

# Keys follow the PRG32 QEMU keyboard mapper: d=RIGHT a=LEFT w=UP s=DOWN
# h=A j=B Enter=SELECT. Every byte holds its button for 120 ms, so a hold
# resends its keys every 100 ms. Timeline (seconds after recording starts):
# title music and piste selection, K2 Crete Rosse, countdown beeps, then
# ~53 s of skiing with effects only: pole pushes off the Crete Rosse hut,
# tucks, carves and brakes down the banked forest corridor and the gully
# S-bend traced from the piste map. MAESTRO assist (on by default) steers
# whenever no Left/Right is held.
TAPS: list[tuple[float, str]] = [
    (1.0, "d"), (1.4, "d"), (1.8, "d"), (2.2, "d"), (2.6, "d"),   # E1 -> M1 -> D4 -> D1 -> T8 -> K2
    (3.4, "h"),                           # start: countdown 3-2-1-VIA!
    (6.5, "j"), (7.0, "j"), (7.5, "j"), (8.0, "j"),   # pole pushes off the start
]
HOLDS: list[tuple[float, float, str]] = []
_t = 8.4
_pattern = ["w", "d", "w", "a", "w", "s", "w", "wa", "w", "wd", "s", "w", "d", "a"]
_i = 0
while _t < 60.0:
    keys = _pattern[_i % len(_pattern)]
    span = 0.35 if keys in ("d", "a") else (0.7 if keys == "s" else (0.4 if len(keys) == 2 else 2.4))
    HOLDS.append((_t, min(60.0, _t + span), keys))
    _t += span + 0.15
    _i += 1
SCRIPT = sorted(TAPS)


def compiled_server(firmware_dir: Path) -> str | None:
    """host:port of CONFIG_PRG32_MULTIPLAYER_SERVER_URL in the QEMU firmware build."""
    cfg = firmware_dir / "config/sdkconfig.h"
    if not cfg.exists():
        return None
    for line in cfg.read_text().splitlines():
        if "CONFIG_PRG32_MULTIPLAYER_SERVER_URL" in line and '"' in line:
            url = line.split('"')[1]
            return url.split("://", 1)[-1].split("/", 1)[0]
    return None


def nic_option(args: argparse.Namespace) -> str:
    """QEMU user network. The firmware can stall while it keeps reconnecting to
    an unreachable room server, so the compiled server address is forwarded
    to a local PRG32 Multiplayer Server (--mp-server) when one is given."""
    nic = "user,model=open_eth"
    target = compiled_server(args.firmware_dir)
    if args.mp_server and target and ":" in target:
        host, port = args.mp_server.rsplit(":", 1)
        # guestfwd needs the forwarded address inside the emulated network,
        # so the user network takes the /24 of the compiled server address.
        octets = target.split(":")[0].split(".")
        if len(octets) == 4 and all(o.isdigit() for o in octets):
            nic += f",net={'.'.join(octets[:3])}.0/24"
        nic += f",guestfwd=tcp:{target}-cmd:nc {host} {port}"
    return nic


class Stall(RuntimeError):
    """The emulator stopped producing audio/video before the capture finished."""


def connect(port: int, proc: subprocess.Popen, timeout: float = 20) -> socket.socket:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited early ({proc.returncode})")
        s = socket.socket()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.connect(("127.0.0.1", port))
            return s
        except ConnectionRefusedError:
            s.close()
            time.sleep(0.1)
    raise TimeoutError(f"port {port}")


POINTER_WATCH = """import Foundation
import CoreGraphics
setvbuf(stdout, nil, _IONBF, 0)
while true {
  let p = CGEvent(source: nil)!.location
  var inside = 0
  let ws = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID)! as NSArray
  for case let w as NSDictionary in ws {
    if w[kCGWindowNumber] as? Int == QEMU_WINDOW, let b = w[kCGWindowBounds] as? NSDictionary,
       let r = CGRect(dictionaryRepresentation: b), r.insetBy(dx: -24, dy: -24).contains(p) { inside = 1 }
  }
  print(inside)
  usleep(50000)
}"""


def corner_position() -> str:
    """Bottom-right corner of the main display, away from where people work."""
    src = "import CoreGraphics\nlet b = CGDisplayBounds(CGMainDisplayID())\nprint(Int(b.width), Int(b.height))"
    try:
        w, h = (int(v) for v in subprocess.run(["/usr/bin/swift", "-e", src], capture_output=True,
                                                text=True, check=True).stdout.split())
        return f"{w - 340},{h - 300}"
    except (subprocess.CalledProcessError, ValueError):
        return "0,0"


def find_window(proc: subprocess.Popen, timeout: float = 20) -> str:
    """CoreGraphics window number of the QEMU SDL window owned by proc."""
    src = f"""import Foundation
import CoreGraphics
let ws = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)! as NSArray
for case let w as NSDictionary in ws {{
  if w[kCGWindowOwnerPID] as? Int == {proc.pid}, let n = w[kCGWindowNumber] as? Int {{ print(n); break }}
}}"""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        out = subprocess.run(["/usr/bin/swift", "-e", src], capture_output=True, text=True).stdout.strip()
        if out:
            return out
        time.sleep(0.2)
    raise TimeoutError("QEMU SDL window not found")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cart", type=Path, default=HERE / "dist/skipass-madness-qemu.prg32")
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--warmup", type=float, default=8.0)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--screenshot-at", type=float, default=30.0)
    ap.add_argument("--preview", type=Path, default=HERE / "assets/preview.mp4")
    ap.add_argument("--screenshot", type=Path, default=HERE / "assets/screenshot.png")
    ap.add_argument("--mp3", type=Path, default=HERE / "assets/preview.mp3")
    ap.add_argument("--icon", type=Path, default=HERE / "assets/icon.png")
    ap.add_argument("--keep", type=Path, help="keep raw video, audio and console log here")
    ap.add_argument("--attempts", type=int, default=4,
                    help="retry a run whose emulator stalls (QEMU firmware can freeze while its "
                         "multiplayer client reconnects to an unreachable room server)")
    ap.add_argument("--allow-pointer", action="store_true",
                    help="accept takes with the host pointer over the window (verification runs only, "
                         "never for Store media)")
    ap.add_argument("--firmware-dir", type=Path, default=SDK / "build-qemu",
                    help="directory holding qemu_flash.bin, qemu_efuse.bin (and config/sdkconfig.h)")
    ap.add_argument("--mp-server", default="127.0.0.1:8081",
                    help="local PRG32 Multiplayer Server (host:port) that receives the firmware's compiled "
                         "server address; empty string to disable")
    ap.add_argument("--keys", help="override the tap script, e.g. '6:h,8:d'")
    args = ap.parse_args()
    if args.keys:
        SCRIPT[:] = sorted((float(t), k) for t, k in (item.split(":") for item in args.keys.split(",")))
    for attempt in range(1, args.attempts + 1):
        try:
            capture(args)
            return
        except Stall as exc:
            print(f"attempt {attempt}/{args.attempts}: {exc}", file=sys.stderr)
    raise SystemExit("capture failed: emulator stalled on every attempt")


def capture(args: argparse.Namespace) -> None:
    ffmpeg = shutil.which("ffmpeg") or sys.exit("ffmpeg is required")

    with tempfile.TemporaryDirectory(prefix="skipass-capture-") as tmp_name:
        tmp = Path(args.keep) if args.keep else Path(tmp_name)
        tmp.mkdir(parents=True, exist_ok=True)
        flash, efuse = tmp / "flash.bin", tmp / "efuse.bin"
        shutil.copy2(args.firmware_dir / "qemu_flash.bin", flash)
        shutil.copy2(args.firmware_dir / "qemu_efuse.bin", efuse)
        subprocess.run([sys.executable, "-m", "prg32", "qemu", "upload", str(args.cart.resolve()),
                        "--flash", str(flash)], cwd=SDK, check=True)
        cmd = ["qemu-system-riscv32", "-M", "esp32c3", "-m", "4M",
               "-drive", f"file={flash},if=mtd,format=raw",
               "-drive", f"file={efuse},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
               "-nic", nic_option(args),          # required: the firmware aborts without its NIC
               "-display", "sdl,show-cursor=off", "-monitor", "none",
               "-serial", f"tcp::{CONSOLE_PORT},server=on,wait=off,nodelay=on",
               "-serial", f"tcp::{AUDIO_PORT},server=on,wait=on,nodelay=on"]
        env = dict(os.environ, SDL_VIDEO_WINDOW_POS=os.environ.get("SKIPASS_WINDOW_POS") or corner_position())
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=(tmp / "qemu.log").open("wb"), env=env)
        pcm = bytearray()
        total = round(args.duration * RATE) * 2
        recording, done = threading.Event(), threading.Event()

        def audio() -> None:
            s = connect(AUDIO_PORT, proc)
            s.sendall(b"K")
            nxt = time.monotonic() + 0.02
            while proc.poll() is None and len(pcm) < total:
                chunk = bytearray()
                while len(chunk) < 882:
                    part = s.recv(882 - len(chunk))
                    if not part:
                        done.set()
                        return
                    chunk.extend(part)
                if recording.is_set():
                    pcm.extend(chunk[:total - len(pcm)])
                delay = nxt - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                nxt += 0.02
                s.sendall(b"K")
            done.set()

        th = threading.Thread(target=audio, daemon=True)
        th.start()
        console = connect(CONSOLE_PORT, proc)
        log = bytearray()

        def reader() -> None:
            console.settimeout(0.2)
            while proc.poll() is None:
                try:
                    data = console.recv(4096)
                except (TimeoutError, OSError):
                    continue
                if not data:
                    return
                log.extend(data)

        threading.Thread(target=reader, daemon=True).start()
        raw = tmp / "window.mov"
        raw.unlink(missing_ok=True)   # screencapture refuses to overwrite
        pointer = None
        try:
            window = find_window(proc)
            # Firmware boots to PRG32 SETUP: RUN CARTRIDGE -> cart0.
            # Wait for the PRG32 SETUP menu (boot time varies with host load),
            # then RUN CARTRIDGE -> cart0 until the cartridge reports loading.
            end = time.monotonic() + 60
            while b"QEMU keyboard input enabled" not in log:
                if time.monotonic() > end:
                    raise Stall("firmware did not reach the setup menu")
                time.sleep(0.1)
            time.sleep(args.warmup / 2)
            for _ in range(4):
                for key in "hh":   # menu waits for release between presses
                    for _ in range(3):
                        console.sendall(key.encode())
                        time.sleep(0.03)
                    time.sleep(2.0)
                if b"loaded cartridge" in log:
                    break
            else:
                raise Stall("cartridge did not start")
            time.sleep(args.warmup / 2)
            # screencapture video always includes the host pointer: watch it and
            # reject (retry) a take where it crosses the QEMU window.
            pointer = subprocess.Popen(["/usr/bin/swift", "-e", POINTER_WATCH.replace("QEMU_WINDOW", window)],
                                       stdout=subprocess.PIPE, text=True)
            samples: list[str] = []
            threading.Thread(target=lambda: samples.extend(pointer.stdout), daemon=True).start()
            recording.set()
            # Record until the emulator has produced `duration` seconds of PCM.
            # QEMU may run slightly slower than wall time while the window is
            # recorded, so the video is later retimed onto the PCM clock.
            started = time.monotonic()
            video = subprocess.Popen(["/usr/sbin/screencapture", "-v", f"-V{args.duration * 1.12 + 3:g}",
                                      "-x", "-o", f"-l{window}", str(raw)])
            event, wall, next_hold = 0, None, 0.0
            while video.poll() is None:
                if wall is None and done.is_set():
                    wall = time.monotonic() - started
                t = len(pcm) / 2 / RATE
                while event < len(SCRIPT) and SCRIPT[event][0] <= t:
                    console.sendall(SCRIPT[event][1].encode())
                    event += 1
                if t >= next_hold:
                    held = "".join(k for a, b, k in HOLDS if a <= t < b)
                    if held:
                        console.sendall(held.encode())
                    next_hold = t + 0.1
                time.sleep(0.01)
            if wall is None:
                wall = time.monotonic() - started
            pointer.terminate()
            if video.returncode != 0 or not raw.exists():
                raise SystemExit(f"screencapture failed ({video.returncode})")
            if len(samples) < args.duration * 5:
                raise SystemExit("pointer watcher did not run (swift CoreGraphics script failed)")
            if not args.allow_pointer and any(s.startswith("1") for s in samples):
                raise Stall("host pointer crossed the QEMU window during recording")
        finally:
            if pointer:
                pointer.terminate()
            console.close()
            proc.terminate()
            proc.wait(timeout=10)
            (tmp / "console.log").write_bytes(bytes(log))
        if len(pcm) < total * 0.98:
            raise Stall(f"PCM capture short: {len(pcm) / 2 / RATE:.2f}s (emulator stalled)")
        wav = tmp / "audio.wav"
        with wave.open(str(wav), "wb") as w:
            w.setparams((1, 2, RATE, len(pcm) // 2, "NONE", "not compressed"))
            w.writeframes(bytes(pcm))
        # The SDL window is the macOS title bar above the 320x240 firmware
        # panel, whose 20-line top and bottom status bands surround the
        # 320x200 cartridge viewport. Window width == 320 * scale.
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                "stream=width,height", "-of", "csv=p=0", str(raw)],
                               capture_output=True, text=True, check=True).stdout.strip()
        width, height = (int(v) for v in probe.split(","))
        scale = width // 320
        if width % 320 or scale < 2:
            raise Stall(f"QEMU window is {width}x{height}, expected a 2x or larger 320-wide panel")
        top = int(os.environ.get("SKIPASS_VIEWPORT_TOP", "-1"))
        if top < 0:
            top = height - scale * 240 + scale * 20
        ratio = (len(pcm) / 2 / RATE) / wall
        crop = (f"setpts=PTS*{ratio:.6f},crop={scale * 320}:{scale * 200}:0:{top},"
                "scale=320:200:flags=neighbor")
        print(f"raw {width}x{height} scale {scale} viewport top {top}; emulator/wall speed {ratio:.4f}")
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(raw), "-i", str(wav),
                        "-t", str(args.duration), "-vf", crop, "-r", str(args.fps), "-af", "apad",
                        "-c:v", "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p",
                        "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart", str(args.preview)],
                       check=True)
        still = f"crop={scale * 320}:{scale * 200}:0:{top},scale=320:200:flags=neighbor"
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-ss", f"{args.screenshot_at / ratio:.3f}",
                        "-i", str(raw), "-frames:v", "1", "-vf", still, str(args.screenshot)], check=True)
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(wav), "-t", str(args.duration),
                        "-c:a", "libmp3lame", "-b:a", "160k", str(args.mp3)], check=True)
        # Icon: a 100x100 square of the same screenshot around the skier, 128x128.
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(args.screenshot), "-vf",
                        "crop=100:100:110:70,scale=128:128:flags=neighbor", str(args.icon)], check=True)
        print(f"preview {args.preview} ({len(pcm) / 2 / RATE:.2f}s PCM), screenshot {args.screenshot}")


if __name__ == "__main__":
    main()
