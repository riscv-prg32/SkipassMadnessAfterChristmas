#!/usr/bin/env python3
"""Two-console multiplayer test for Skipass Madness against a local server.

Start a PRG32 Multiplayer Server first (PRG32-Multiplayer-Server: npm start,
listening on 127.0.0.1:8081), then run, after ``source $HOME/esp-idf/export.sh``:

    python3 scripts/multiplayer_test.py --out /tmp/skipass-mp

The script

* boots two QEMU consoles (SDL windows, audio unpaced and silent), each from its own copy of the PRG32 QEMU
  flash with its NVS erased (so each has its own multiplayer identity) and
  only the freshly built QEMU cartridge in cart0;
* forwards the firmware's compiled server address to the local server with
  QEMU guestfwd, exactly as scripts/capture_media.py does;
* joins the room silently as an observer (it publishes nothing, so consoles
  never see it) and records every snapshot the server relays;
* starts the cartridge on both consoles, selects the same piste and starts
  the run on both, then lets them ski (console 1 tucks, console 2 is pushed
  less) for --duration seconds;
* dumps both framebuffers through the QEMU monitor (title and mid-run) so the
  rival rendering, "NET ROOM: 2 SKIERS ONLINE" and the "NET P1/2" rank can be
  inspected, and writes report.json.

Checks (all must pass):
  1. both consoles join the room and publish snapshots;
  2. both publish mode PLAY on the same course while skiing;
  3. each console's along-course position (y) advances during the run;
  4. the server reports no errors.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import urlparse

HERE = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("PRG32_REPO", HERE.parent / "PRG32")).resolve()
ROOM = "skipass-madness-roccaraso-v2"
NVS_OFFSET, NVS_SIZE = 0x9000, 0x6000
MODES = {0: "TITLE", 2: "COUNT", 3: "PLAY", 4: "CRASH", 5: "WIN", 6: "OVER"}
sys.path.insert(0, str(HERE / "scripts"))
from capture_media import compiled_server  # noqa: E402


class WebSocket:
    """Minimal RFC 6455 client: masked text frames out, text frames in."""

    def __init__(self, url: str, timeout: float = 5.0) -> None:
        u = urlparse(url)
        self.sock = socket.create_connection((u.hostname, u.port or 80), timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {u.path or '/'} HTTP/1.1\r\nHost: {u.netloc}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during upgrade")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(head[:80])

    def send(self, msg: dict) -> None:
        data = json.dumps(msg).encode()
        mask = os.urandom(4)
        head = struct.pack("!BB", 0x81, 0x80 | len(data)) if len(data) < 126 else struct.pack("!BBH", 0x81, 0xFE, len(data))
        self.sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _need(self, n: int) -> bytes:
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def receive(self) -> dict | None:
        a, b = self._need(2)
        n = b & 0x7F
        if n == 126:
            n = struct.unpack("!H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self._need(8))[0]
        payload = self._need(n)
        return json.loads(payload) if a & 0x0F == 1 else None


def observe(url: str, samples: list, errors: list, stop: threading.Event) -> None:
    ws = WebSocket(url)
    ws.sock.settimeout(1.0)
    ws.send({"type": "join", "signature": ROOM, "protocol": "prg32-multiplayer-1.0"})
    last_ping = time.monotonic()
    while not stop.is_set():
        if time.monotonic() - last_ping > 5:
            ws.send({"type": "ping"})
            last_ping = time.monotonic()
        try:
            m = ws.receive()
        except (socket.timeout, TimeoutError):
            continue
        if not m:
            continue
        if m.get("type") == "peer":
            samples.append({"t": time.monotonic(), **{k: m.get(k) for k in ("player_id", "x", "y", "sprite", "flags", "platform")}})
        elif m.get("type") == "error":
            errors.append(m.get("code", "?"))
    ws.sock.close()


class Console:
    def __init__(self, n: int, out: Path, args: argparse.Namespace) -> None:
        self.n = n
        self.dir = out / f"qemu{n}"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.flash = self.dir / "flash.bin"
        shutil.copy2(args.firmware_dir / "qemu_flash.bin", self.flash)
        shutil.copy2(args.firmware_dir / "qemu_efuse.bin", self.dir / "efuse.bin")
        with self.flash.open("r+b") as f:   # new multiplayer identity
            f.seek(NVS_OFFSET)
            f.write(b"\xff" * NVS_SIZE)
        subprocess.run([sys.executable, "-m", "prg32", "qemu", "upload", str(args.cart.resolve()), "--flash", str(self.flash)],
                       cwd=SDK, check=True, stdout=subprocess.DEVNULL)
        self.console_port = 4500 + n * 10
        self.monitor_port = 4501 + n * 10
        nic = "user,model=open_eth"
        target = compiled_server(args.firmware_dir)
        if target:
            o = target.split(":")[0].split(".")
            nic += f",net={'.'.join(o[:3])}.0/24,guestfwd=tcp:{target}-cmd:nc {args.mp_server.replace(':', ' ')}"
        cmd = ["qemu-system-riscv32", "-M", "esp32c3", "-m", "4M",
               "-drive", f"file={self.flash},if=mtd,format=raw",
               "-drive", f"file={self.dir / 'efuse.bin'},if=none,format=raw,id=efuse",
               "-global", "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
               "-global", "driver=timer.esp32c3.timg,property=wdt_disable,value=true",
               "-nic", nic, "-display", "sdl",
               "-monitor", f"tcp:127.0.0.1:{self.monitor_port},server=on,wait=off",
               "-serial", f"tcp::{self.console_port},server=on,wait=off,nodelay=on",
               "-serial", f"tcp:127.0.0.1:{4502 + n * 10},server=on,wait=off,nodelay=on"]   # audio: no player
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=(self.dir / "qemu.log").open("wb"))
        self.log = bytearray()
        end = time.monotonic() + 20
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.console_port), 1)
                break
            except OSError:
                if time.monotonic() > end or self.proc.poll() is not None:
                    raise RuntimeError(f"console {n} did not start")
                time.sleep(0.2)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        self.sock.settimeout(0.5)
        while self.proc.poll() is None:
            try:
                d = self.sock.recv(4096)
            except (TimeoutError, OSError):
                continue
            if not d:
                return
            self.log.extend(d)

    def keys(self, k: str, repeat: int = 1) -> None:
        for _ in range(repeat):
            self.sock.sendall(k.encode())
            time.sleep(0.1)

    def wait_for(self, text: bytes, seconds: float) -> bool:
        end = time.monotonic() + seconds
        while text not in self.log:
            if time.monotonic() > end:
                return False
            time.sleep(0.1)
        return True

    def monitor(self, command: str) -> str:
        with socket.create_connection(("127.0.0.1", self.monitor_port), 5) as s:
            s.settimeout(0.5)
            s.sendall(command.encode() + b"\n")
            time.sleep(0.3)
            reply = b""
            try:
                while True:
                    c = s.recv(65536)
                    if not c:
                        break
                    reply += c
            except (TimeoutError, OSError):
                pass
        return reply.decode("latin1")

    def screenshot(self, g_fb: int, png: Path) -> None:
        reply = self.monitor(f"xp /1xw 0x{g_fb:x}")
        addr = None
        for line in reply.splitlines():
            if ":" in line and line.strip().lower().lstrip("0").startswith(f"{g_fb:x}"):
                addr = int(line.split(":")[1].split()[0], 16)
        if addr is None:
            print(f"console {self.n}: framebuffer pointer not readable")
            return
        raw = self.dir / "fb.rgb565"
        raw.unlink(missing_ok=True)
        self.monitor(f'pmemsave 0x{addr:x} {320 * 240 * 2} "{raw}"')
        for _ in range(30):
            if raw.exists() and raw.stat().st_size == 320 * 240 * 2:
                break
            time.sleep(0.1)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb565le", "-s", "320x240",
                        "-i", str(raw), "-vf", "scale=640:480:flags=neighbor", str(png)], check=False)

    def close(self) -> None:
        try:
            self.sock.close()
        finally:
            self.proc.terminate()
            self.proc.wait(timeout=10)
            (self.dir / "console.log").write_bytes(bytes(self.log))


def g_fb_address(elf: Path) -> int:
    nm = shutil.which("riscv32-esp-elf-nm") or "nm"
    for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == "g_fb":
            return int(p[0], 16)
    raise SystemExit("g_fb not found")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cart", type=Path, default=HERE / "dist/skipass-madness-qemu.prg32")
    ap.add_argument("--firmware-dir", type=Path, default=SDK / "build-qemu")
    ap.add_argument("--mp-server", default="127.0.0.1:8081")
    ap.add_argument("--course", type=int, default=0, help="piste index on the title list (0 = E1)")
    ap.add_argument("--duration", type=float, default=25.0)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    samples: list = []
    errors: list = []
    stop = threading.Event()
    threading.Thread(target=observe, args=(f"ws://{args.mp_server}/", samples, errors, stop), daemon=True).start()
    consoles: list[Console] = []
    checks: list[tuple[str, bool, str]] = []
    try:
        consoles = [Console(1, args.out, args), Console(2, args.out, args)]
        for c in consoles:
            if not c.wait_for(b"QEMU keyboard input enabled", 60):
                raise SystemExit(f"console {c.n} did not reach the setup menu")
        time.sleep(3)
        for c in consoles:   # PRG32 SETUP: RUN CARTRIDGE -> cart0
            for _ in range(4):
                c.keys("h", 3)
                time.sleep(2)
                if b"loaded cartridge" in c.log:
                    break
        for c in consoles:
            if not c.wait_for(b"joined " + ROOM.encode(), 30):
                print(f"console {c.n}: no join line in its log")
        time.sleep(4)
        g_fb = g_fb_address(args.firmware_dir / "PRG32.elf")
        consoles[0].screenshot(g_fb, args.out / "title-console1.png")
        for c in consoles:
            c.keys("d", args.course)
            time.sleep(0.4)
        for c in consoles:
            c.keys("h")
        t0 = time.monotonic()
        shot = False
        while time.monotonic() - t0 < args.duration + 4:
            el = time.monotonic() - t0
            if 3.3 < el < 5.5:
                for c in consoles:
                    c.keys("j")
            elif el >= 5.5:
                consoles[0].keys("w")
                if int(el * 2) % 3 == 0:
                    consoles[1].keys("w")
                else:
                    time.sleep(0.1)
            if not shot and el > 4 + args.duration * 0.6:
                for c in consoles:
                    c.screenshot(g_fb, args.out / f"run-console{c.n}.png")
                shot = True
    finally:
        stop.set()
        for c in consoles:
            c.close()
    ids = sorted({s["player_id"] for s in samples})
    checks.append(("both consoles publish snapshots", len(ids) >= 2, f"player ids {ids}"))
    play = [s for s in samples if s["flags"] is not None and s["flags"] & 15 == 3]
    per = {pid: [s for s in play if s["player_id"] == pid] for pid in ids}
    courses = {s["sprite"] & 15 for s in play}
    checks.append(("both race in PLAY on the same piste", all(per[p] for p in ids) and len(ids) >= 2 and courses == {args.course},
                   f"PLAY snapshots {[len(per[p]) for p in ids]}, courses {sorted(courses)}"))
    adv = {p: (v[-1]["y"] - v[0]["y"]) * 20 // 100 if v else 0 for p, v in per.items()}
    checks.append(("positions advance down the piste", len(adv) >= 2 and all(m > 30 for m in adv.values()), f"metres {adv}"))
    checks.append(("no server errors", not errors, ", ".join(errors) or "none"))
    report = {"room": ROOM, "samples": len(samples), "players": ids,
              "last": {p: (per[p][-1] if per[p] else None) for p in ids},
              "checks": [{"name": n, "passed": ok, "detail": d} for n, ok, d in checks]}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for n, ok, d in checks:
        print(f"{'PASS' if ok else 'FAIL'}  {n}: {d}")
    ok = all(c[1] for c in checks)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
