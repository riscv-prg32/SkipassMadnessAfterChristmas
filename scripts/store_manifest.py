#!/usr/bin/env python3
"""Write the Cartridge Store bundle manifest from metadata.json and colophon.json.

Cartridge Store intake requires manifest.abi == prg32-metadata-1.0. It rebuilds
every architecture's cartridge from the manifest: metadata fields from the
manifest itself, the screenshot from assets.splash and the colophon from the
inline colophon object. assets.preview is carried for the Store page.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "manifest.json"
meta = json.loads((ROOT / "metadata.json").read_text())
colophon = json.loads((ROOT / "colophon.json").read_text())
if meta["abi"] != "prg32-metadata-1.0":
    sys.exit("metadata.json abi must be prg32-metadata-1.0")
if colophon["title"] != meta["title"] or colophon["version"] != meta["version"]:
    sys.exit("colophon.json title/version must match metadata.json")

manifest = {k: v for k, v in meta.items() if k != "assets"}
manifest["architectures"] = [
    {"id": a["id"], "file": f"skipass-madness-{a['id']}.prg32"} for a in meta["runtime"]["architectures"]
]
manifest["assets"] = {"icon": "icon.png", "splash": "screenshot.png", "preview": "preview.mp4"}
manifest["colophon"] = colophon
out.write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n")
print(f"wrote {out}")
