#!/usr/bin/env python3
"""Small review images for the simulator output.

The sim writes lossless 400x300 grayscale PNGs (~120 KB each). Shown at full
size they flood the context, so this makes the two things worth looking at:

  tools/preview.py sheet            labelled contact sheet of every scenario
  tools/preview.py zoom NAME        2x crop of a region (default: the glass card)

  .venv/bin/python tools/preview.py sheet out
  .venv/bin/python tools/preview.py zoom rain out --region card
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 400, 300
# Regions of interest in canvas coordinates.
REGIONS = {
    "card": (10, 150, 390, 292),      # the glass card + forecast row
    "clock": (18, 158, 250, 232),     # time + date
    "sky": (0, 0, 400, 150),          # sky, sun/moon, hills
    "chip": (250, 150, 392, 292),     # right-hand detail column
    "full": (0, 0, 400, 300),
}


def scenarios(out: Path, suffix: str = "16lvl") -> list[str]:
    """Scenario names, ignoring our own sheets and zoom crops."""
    names = set()
    for p in out.glob(f"*_{suffix}.png"):
        name = p.name[: -len(f"_{suffix}.png")]
        if name.startswith("sheet") or name.endswith("_zoom") or name == "glyphs":
            continue
        names.add(name)
    return sorted(names)


def sheet(out: Path, scale: float, cols: int, suffix: str = "16lvl") -> Path:
    names = scenarios(out, suffix)
    if not names:
        sys.exit(f"no *_{suffix}.png in {out}")
    tw, th = int(W * scale), int(H * scale)
    rows = (len(names) + cols - 1) // cols
    pad, label_h = 8, 16
    img = Image.new("L", (cols * (tw + pad) + pad, rows * (th + label_h + pad) + pad), 40)
    draw = ImageDraw.Draw(img)
    for i, name in enumerate(names):
        src = out / f"{name}_{suffix}.png"
        tile = Image.open(src).resize((tw, th), Image.NEAREST)
        x = pad + (i % cols) * (tw + pad)
        y = pad + (i // cols) * (th + label_h + pad)
        img.paste(tile, (x, y))
        draw.text((x + 2, y + th + 2), name, fill=255)
    dest = out / ("sheet.png" if suffix == "16lvl" else f"sheet_{suffix}.png")
    img.save(dest)
    print(f"{dest}  ({len(names)} scenarios, {img.size[0]}x{img.size[1]})")
    return dest


def zoom(out: Path, name: str, region: str, scale: float) -> Path:
    src = out / f"{name}_16lvl.png"
    if not src.exists():
        sys.exit(f"missing {src}")
    box = REGIONS[region]
    crop = Image.open(src).crop(box)
    crop = crop.resize((int(crop.width * scale), int(crop.height * scale)), Image.NEAREST)
    dest = out / f"{name}_{region}_zoom.png"
    crop.save(dest)
    print(f"{dest}  ({crop.size[0]}x{crop.size[1]})")
    return dest


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["sheet", "zoom"])
    ap.add_argument("target", nargs="?", default="")
    ap.add_argument("out", nargs="?", default="sim/out")
    ap.add_argument("--region", default="card", choices=sorted(REGIONS))
    ap.add_argument("--scale", type=float, default=None)
    ap.add_argument("--cols", type=int, default=5)
    ap.add_argument("--suffix", default="16lvl",
                    help="image variant to tile, e.g. 16lvl or sky_16lvl")
    a = ap.parse_args()

    out = Path(a.out)
    if a.mode == "sheet":
        sheet(out, a.scale or 0.5, a.cols, a.suffix)
    else:
        if not a.target:
            sys.exit("zoom needs a scenario name")
        # target may have landed in the out slot when out was omitted
        if a.out == "sim/out" and not out.exists() and (Path("sim/out") / f"{a.target}_16lvl.png").exists():
            out = Path("sim/out")
        zoom(out, a.target, a.region, a.scale or 2.0)


if __name__ == "__main__":
    main()
