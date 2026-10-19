#!/usr/bin/env python3
"""Render the baked glyph coverage back to a PNG so data and rasterizer can be
told apart.

If a glyph looks wrong *here*, tools/gen_assets.py baked it wrong; if it looks
right here but wrong in the simulator, the bug is in glass_font/gc_text.

    .venv/bin/python tools/check_glyphs.py f10b "Today Wed 29 dry ?°·"
    .venv/bin/python tools/check_glyphs.py f90b "0123456789" --row
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw

GEN = Path(__file__).resolve().parents[1] / "firmware/components/glass_gfx/generated/glass_assets.cc"
CELL_PAD = 4


def parse_face(src: str, face: str) -> dict[int, dict]:
    table = re.search(r"static const glass_glyph_t glyphs_%s\[\] = \{(.*?)\n\};" % face,
                      src, re.S)
    if not table:
        raise SystemExit(f"no face {face}")
    out: dict[int, dict] = {}
    for line in table.group(1).strip().splitlines():
        cp, w, h, left, top, adv, bits = line.strip().rstrip(",").strip("{}").split(", ")
        cp = int(cp)
        cov = None
        if bits != "nullptr":
            arr = re.search(r"static const uint8_t %s\[\] = \{(.*?)\n\};" % bits, src, re.S)
            cov = [int(v) for v in arr.group(1).replace("\n", " ").split(",") if v.strip()]
        out[cp] = {"w": int(w), "h": int(h), "left": int(left), "top": int(top),
                   "adv": int(adv), "cov": cov}
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("face")
    ap.add_argument("text", nargs="?", default="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")
    ap.add_argument("--scale", type=int, default=6)
    ap.add_argument("--out", default="sim/out/glyphs.png")
    a = ap.parse_args()

    src = GEN.read_text()
    glyphs = parse_face(src, a.face)
    s = a.scale
    text = a.text
    cps = [ord(c) for c in text]

    # One cell per codepoint; show the coverage, plus the requested char and
    # the codepoint actually used, so a lookup bug is obvious.
    cell_w = max((g["adv"] or 6 for g in glyphs.values()), default=8) + CELL_PAD * 2
    cell_h = max((g["h"] for g in glyphs.values()), default=10) + CELL_PAD * 3 + 10
    img = Image.new("L", (cell_w * len(cps), cell_h), 240)
    draw = ImageDraw.Draw(img)
    for i, cp in enumerate(cps):
        g = glyphs.get(cp)
        x0 = i * cell_w + CELL_PAD
        y0 = CELL_PAD
        draw.rectangle((i * cell_w, 0, i * cell_w + cell_w - 1, cell_h - 1), outline=170)
        if g is None:
            draw.text((x0, y0), "?", fill=0)
            continue
        if g["cov"] is None:
            draw.text((x0, y0), "(blank)", fill=120)
        else:
            expected = g["w"] * g["h"]
            ok = len(g["cov"]) == expected
            for row in range(g["h"]):
                for col in range(g["w"]):
                    v = g["cov"][row * g["w"] + col]
                    if v:
                        # ink is dark on light, like the firmware draws it
                        x = x0 + g["left"] + col
                        y = y0 + g["top"] + row
                        draw.point((x, y), fill=255 - v)
            if not ok:
                draw.text((x0, cell_h - 12), f"len {len(g['cov'])}!={expected}", fill=200)
        draw.text((x0, cell_h - 12), f"U+{cp:04X}", fill=100)
    dest = Path(a.out)
    dest.parent.mkdir(parents=True, exist_ok=True)
    img = img.resize((img.width * s, img.height * s), Image.NEAREST)
    img.save(dest)
    print(f"{dest}  face={a.face} '{text}' scale={s} ({img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
