#!/usr/bin/env python3
"""Bake fonts, weather icons and the WMO code table into C headers.

Run from the repo root:

    .venv/bin/python tools/gen_assets.py

Everything here is a build-time step. The firmware only ever sees the
generated headers under firmware/components/glass_gfx/generated/, so the
device never needs a font rasterizer or a path engine.

Icon geometry is vendored from itopinion/zectrix-weather-glass (MIT); see
vendor/zectrix-weather-glass/ and docs/ATTRIBUTION.md.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor" / "zectrix-weather-glass"
OUT = ROOT / "firmware" / "components" / "glass_gfx" / "generated"
APP_OUT = ROOT / "firmware" / "components" / "glass_app" / "generated"
FONT_PATH = ROOT / "assets" / "Inter.ttf"

sys.path.insert(0, str(VENDOR))
import geometry as vec  # noqa: E402  (vendored MIT vector primitives)

# ---------------------------------------------------------------------------
# Font baking
# ---------------------------------------------------------------------------

# Codepoints we render: printable ASCII plus the two Latin-1 marks the layout
# uses (degree sign and middot). Anything else draws as a fallback box.
BASE_CODEPOINTS = list(range(32, 127)) + [0xB0, 0xB7]
# The oversized faces draw the temperature, which is always digits plus a unit.
# 'C'/'F' have to be in here: leaving them out drew a fallback box after the
# degree sign, because only these faces carry a reduced table.
DIGIT_CODEPOINTS = [ord(c) for c in "0123456789-.CF"] + [0xB0]

# name -> (pixel size, bold, codepoints)
FACES = [
    ("f9r", 9, False, BASE_CODEPOINTS),
    ("f10r", 10, False, BASE_CODEPOINTS),
    ("f12r", 12, False, BASE_CODEPOINTS),
    ("f17r", 17, False, BASE_CODEPOINTS),
    ("f9b", 9, True, BASE_CODEPOINTS),
    ("f10b", 10, True, BASE_CODEPOINTS),
    ("f12b", 12, True, BASE_CODEPOINTS),
    ("f17b", 17, True, BASE_CODEPOINTS),
    ("f23r", 23, False, DIGIT_CODEPOINTS),
    ("f55b", 55, True, DIGIT_CODEPOINTS),
    ("f73b", 73, True, DIGIT_CODEPOINTS),
    ("f90b", 90, True, DIGIT_CODEPOINTS),
]

# Rasterize at 4x then box-filter down: cheap, and it gives coverage values
# that match what the reference renderer's 3x supersampling produced.
SS = 4


def load_font(size: int, bold: bool) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(FONT_PATH), size * SS)
    axes = font.get_variation_axes()
    targets = []
    for axis in axes:
        if axis["name"] == b"Weight":
            targets.append(700 if bold else 450)
        elif axis["name"] == b"Optical size":
            targets.append(min(32, max(14, size)))
        else:
            targets.append(axis["default"])
    if axes:
        font.set_variation_by_axes(targets)
    return font


def bake_face(name: str, size: int, bold: bool, codepoints: list[int]) -> dict:
    """Bake one face.

    Glyphs are rasterized at 4x with the pen at (pad, pad) of the downsampled
    canvas, then box-filtered to 1x. `left`/`top` are offsets from that pen
    position.

    The anchor must be "la" (left of the pen, *ascender* line), not "lt": "lt"
    anchors the tight bounding box, which puts every glyph's ink box at the pen
    and throws the baseline away - lowercase then renders at cap height, so
    "Today" reads as "T deg day" and "Wed" as "W?d".
    """
    font = load_font(size, bold)
    ascent, descent = font.getmetrics()
    pad = 4
    canvas = (size + 2 * pad + 4) * SS
    glyphs = []
    for cp in codepoints:
        ch = chr(cp)
        advance = max(1, round(font.getlength(ch) / SS))
        big = Image.new("L", (canvas, canvas), 0)
        ImageDraw.Draw(big).text((pad * SS, pad * SS), ch, font=font, fill=255,
                                anchor="la")
        small = big.resize((canvas // SS, canvas // SS), Image.Resampling.BOX)
        box = small.getbbox()
        if box is None:  # space and other blank glyphs
            glyphs.append({"cp": cp, "w": 0, "h": 0, "left": 0, "top": 0,
                           "advance": advance, "bits": []})
            continue
        crop = small.crop(box)
        glyphs.append({
            "cp": cp,
            "w": crop.width,
            "h": crop.height,
            "left": box[0] - pad,
            "top": box[1] - pad,
            "advance": advance,
            "bits": list(crop.tobytes()),
        })
    return {
        "name": name,
        "size": size,
        "ascent": round(ascent / SS),
        "descent": round(descent / SS),
        "glyphs": glyphs,
    }


# ---------------------------------------------------------------------------
# Icon baking (vendored vector geometry)
# ---------------------------------------------------------------------------

# Kind names accepted by the vendored shapes() table.
ICON_KINDS = [
    "sunny", "partly_cloudy", "cloudy", "overcast", "clear_night",
    "rain", "drizzle", "showers", "snow", "sleet", "ice", "thunder",
    "fog", "mist", "smog", "dust_haze", "blowing_snow", "blizzard",
    "wind", "humidity", "unknown",
]
# Hero sizes worth the flash: 103 px matches the reference layout's main icon.
HERO_KINDS = [
    "sunny", "partly_cloudy", "cloudy", "overcast", "clear_night",
    "rain", "drizzle", "showers", "snow", "sleet", "ice", "thunder",
    "fog", "mist", "smog", "dust_haze", "blowing_snow", "blizzard",
    "unknown",
]
ICON_SIZES = {"hero": 103, "small": 33, "metric": 20}


def shapes(kind: str, day: bool) -> list[dict]:
    """Ported from zectrix-weather-glass src/icons.py (MIT)."""
    if kind == "wind":
        return [
            vec.path("M 8 22 L 35 22 C 48 22 48 8 40 8 C 35 8 34 12 35 15", width=3),
            vec.path("M 8 32 L 48 32 C 60 32 60 19 52 19 C 48 19 47 22 48 24", width=3),
            vec.path("M 8 42 L 31 42 C 43 42 43 55 35 55 C 30 55 29 51 30 49", width=3),
        ]
    if kind == "humidity":
        return [vec.path("M 32 7 C 27 17 15 29 15 40 C 15 60 49 60 49 40 "
                         "C 49 29 37 17 32 7 Z", width=3)]
    if kind == "unknown":
        return [vec.circle(32, 32, 23),
                vec.path("M 24 25 C 24 13 44 13 43 25 C 43 32 32 31 32 38"),
                vec.circle(32, 46, 1.4, fill=255)]
    if kind == "partly_cloudy":
        sun = vec._sun(24, 24, 10) if day else moon()
        return sun + [vec.path(vec.CLOUD, fill=0, transform=(1, 1, 0, 7))]
    if kind in ("cloudy", "overcast"):
        return vec.shapes("cloudy")
    if kind == "clear_night":
        return vec.shapes("clear_night")
    if kind == "sunny":
        return vec.shapes("sunny")

    obscured = kind in {
        "haze", "dust_haze", "blowing_dust", "dust_storm", "sandstorm",
        "mist", "smoke", "smoky_haze", "smog", "severe_smog", "dust", "fog",
        "freezing_fog",
    }
    if obscured:
        if kind in ("fog", "mist", "freezing_fog", "smog", "severe_smog"):
            out = [vec.path(vec.CLOUD, fill=0, transform=(0.85, 0.85, 4, -4))]
        else:
            out = list(vec._sun(32, 20, 9) if day else moon())
        for i, y in enumerate([40, 47, 54]):
            out.append(vec.path(f"M {10 + i % 2 * 5} {y} L {54 - i % 2 * 5} {y}",
                                width=2.8))
        if any(x in kind for x in ["dust", "sand"]):
            for x, y in [(14, 36), (27, 43), (42, 50), (50, 36)]:
                out.append(vec.circle(x, y, 1, fill=255))
        return out

    patchy = "patchy" in kind or "showers" in kind
    out = list(vec._sun(20, 16, 7) if day else moon()) if patchy else []
    out.append(vec.path(vec.CLOUD, fill=0, transform=(1, 1, 0, -6)))
    heavy = any(x in kind for x in ["heavy", "moderate", "torrential", "blizzard"])
    xs = [18, 33, 48] if heavy else [24, 42]
    if "thunder" in kind:
        out.append(vec.path("M 30 40 L 22 51 L 30 51 L 27 61 L 43 46 L 35 46 "
                            "L 39 40 Z", fill=255, stroke=None))
        xs = [12, 52]
    if "snow" in kind or kind == "blizzard":
        for x in xs:
            out += snowflake(x, 51)
    elif "sleet" in kind:
        out.append(vec._drop(20, 45))
        out += snowflake(44, 51)
    elif "ice" in kind:
        for x in xs:
            out.append(vec.path(f"M {x} 46 L {x + 3} 50 L {x + 2} 54 L {x - 2} 54 "
                                f"L {x - 3} 50 Z", fill=255, stroke=None))
    elif "drizzle" in kind:
        for x in xs:
            out += [vec.circle(x, 47, 1.4, fill=255, stroke=None),
                    vec.circle(x, 54, 1.4, fill=255, stroke=None)]
    elif "rain" in kind:
        for x in xs:
            out.append(vec._drop(x, 44))
    if kind in ("blowing_snow", "blizzard"):
        out.append(vec.path("M 8 44 L 49 44 C 60 44 60 34 53 35", width=2.2))
    return out


def snowflake(x: int, y: int, r: float = 4) -> list[dict]:
    return [vec.path(f"M {x - r * math.cos(a)} {y - r * math.sin(a)} "
                     f"L {x + r * math.cos(a)} {y + r * math.sin(a)}", width=2.2)
            for a in (0, math.pi / 3, 2 * math.pi / 3)]


def moon() -> list[dict]:
    out = vec.shapes("clear_night")
    for shape in out:
        shape["transform"] = (0.50, 0.50, 8, 0)
    return out


def rasterize(shape_list: list[dict], size: int) -> Image.Image:
    """Ported from zectrix-weather-glass src/icons.py mask() (MIT)."""
    scale = size * 4 / 64
    im = Image.new("L", (size * 4, size * 4), 0)
    draw = ImageDraw.Draw(im)
    for shape in shape_list:
        sx, sy, tx, ty = shape.get("transform") or (1, 1, 0, 0)
        width = max(1, round(shape["width"] * scale * math.sqrt(sx * sy)))
        if shape["type"] == "circle":
            x = (shape["cx"] * sx + tx) * scale
            y = (shape["cy"] * sy + ty) * scale
            r = shape["r"] * scale * sx
            if shape["fill"] is not None:
                draw.ellipse((x - r, y - r, x + r, y + r), fill=shape["fill"])
            if shape["stroke"] is not None:
                draw.ellipse((x - r - width / 2, y - r - width / 2,
                              x + r + width / 2, y + r + width / 2),
                             fill=shape["stroke"])
                if r > width / 2:
                    draw.ellipse((x - r + width / 2, y - r + width / 2,
                                  x + r - width / 2, y + r - width / 2),
                                 fill=shape["fill"] or 0)
            continue
        pts, _closed = vec._flatten(shape["d"])
        pts = [((x * sx + tx) * scale, (y * sy + ty) * scale) for x, y in pts]
        if shape["fill"] is not None:
            draw.polygon(pts, fill=shape["fill"])
        if shape["stroke"] is not None:
            draw.line(pts, fill=shape["stroke"], width=width, joint="curve")
            # Rounded joins at every sampled vertex prevent pinholes in wide
            # cubic strokes (upstream comment, kept for the same reason).
            for x, y in pts:
                draw.ellipse((x - width / 2, y - width / 2, x + width / 2,
                              y + width / 2), fill=shape["stroke"])
    return im.resize((size, size), Image.Resampling.LANCZOS)


# ---------------------------------------------------------------------------
# WMO (Open-Meteo) -> icon kind, sky scene, label
# ---------------------------------------------------------------------------

# (code, icon_kind, scene, label, precip, cloud_pct_override_or_None)
WMO_TABLE = [
    (0,  "sunny",       "clear",  "Clear",          "none", None),
    (1,  "partly_cloudy", "cloud", "Mainly clear",   "none", 20),
    (2,  "partly_cloudy", "cloud", "Partly cloudy",  "none", 45),
    (3,  "overcast",    "cloud",  "Overcast",       "none", 95),
    (45, "fog",         "fog",    "Fog",            "none", 70),
    (48, "fog",         "fog",    "Rime fog",       "none", 80),
    (51, "drizzle",     "rain",   "Light drizzle",  "drizzle", 85),
    (53, "drizzle",     "rain",   "Drizzle",        "drizzle", 88),
    (55, "drizzle",     "rain",   "Dense drizzle",  "drizzle", 92),
    (56, "sleet",       "rain",   "Freezing drizzle", "drizzle", 92),
    (57, "sleet",       "rain",   "Freezing drizzle", "drizzle", 95),
    (61, "rain",        "rain",   "Light rain",     "rain", 90),
    (63, "rain",        "rain",   "Rain",           "rain", 94),
    (65, "rain",        "rain",   "Heavy rain",     "rain", 98),
    (66, "sleet",       "rain",   "Freezing rain",  "rain", 95),
    (67, "sleet",       "rain",   "Freezing rain",  "rain", 98),
    (71, "snow",        "snow",   "Light snow",     "snow", 90),
    (73, "snow",        "snow",   "Snow",           "snow", 94),
    (75, "snow",        "snow",   "Heavy snow",     "snow", 98),
    (77, "snow",        "snow",   "Snow grains",    "snow", 90),
    (80, "showers",     "rain",   "Light showers",  "rain", 80),
    (81, "showers",     "rain",   "Showers",        "rain", 88),
    (82, "showers",     "rain",   "Violent showers", "rain", 96),
    (85, "snow",        "snow",   "Snow showers",   "snow", 88),
    (86, "snow",        "snow",   "Heavy snow showers", "snow", 94),
    (95, "thunder",     "thunder", "Thunderstorm",  "rain", 97),
    (96, "thunder",     "thunder", "Storm, hail",   "hail", 98),
    (99, "thunder",     "thunder", "Severe storm",  "hail", 99),
]


# ---------------------------------------------------------------------------
# Emit headers
# ---------------------------------------------------------------------------

def c_bytes(values: list[int], per_line: int = 20) -> str:
    rows = []
    for i in range(0, len(values), per_line):
        rows.append("    " + ", ".join(str(v) for v in values[i:i + per_line]))
    return ",\n".join(rows) if rows else "0"


def emit_font(faces: list[dict]) -> tuple[str, str, int]:
    """Return (header declarations, translation-unit body, byte count)."""
    decl = [
        "// Font faces baked from Inter (SIL OFL 1.1).",
    ]
    body = []
    total = 0
    for face in faces:
        for glyph in face["glyphs"]:
            if not glyph["bits"]:
                continue
            body.append(f"static const uint8_t glyph_{face['name']}_{glyph['cp']}[] = {{")
            body.append(c_bytes(glyph["bits"]))
            body.append("};")
            total += len(glyph["bits"])
        body.append(f"static const glass_glyph_t glyphs_{face['name']}[] = {{")
        for glyph in face["glyphs"]:
            bits = f"glyph_{face['name']}_{glyph['cp']}" if glyph["bits"] else "nullptr"
            body.append(
                f"    {{{glyph['cp']}, {glyph['w']}, {glyph['h']}, "
                f"{glyph['left']}, {glyph['top']}, {glyph['advance']}, {bits}}},")
        body.append("};")
        body.append(f"const glass_font_t glass_font_{face['name']} = {{")
        body.append(f"    .glyphs = glyphs_{face['name']},")
        body.append(f"    .count = {len(face['glyphs'])},")
        body.append(f"    .size = {face['size']},")
        body.append(f"    .ascent = {face['ascent']},")
        body.append(f"    .descent = {face['descent']},")
        body.append("};")
        body.append("")
        decl.append(f"extern const glass_font_t glass_font_{face['name']};")
    decl.append(f"// {total} bytes of baked glyph coverage.")
    return "\n".join(decl) + "\n", "\n".join(body) + "\n", total


def emit_icons() -> tuple[str, str, int]:
    decl = [
        "// Weather icon masks rasterized from itopinion/zectrix-weather-glass",
        "// vector geometry (MIT); see docs/ATTRIBUTION.md.",
    ]
    body = []
    total = 0
    entries = []
    for kind in ICON_KINDS:
        for day in (True, False):
            variants = []
            for slot, size in (("hero", ICON_SIZES["hero"]),
                               ("small", ICON_SIZES["small"]),
                               ("metric", ICON_SIZES["metric"])):
                if slot == "hero" and kind not in HERO_KINDS:
                    continue
                mask = rasterize(shapes(kind, day), size)
                name = f"icon_{kind}_{'day' if day else 'night'}_{slot}"
                body.append(f"static const uint8_t {name}[] = {{")
                body.append(c_bytes(list(mask.tobytes())))
                body.append("};")
                variants.append((name, size))
                total += size * size
            suffix = "day" if day else "night"
            body.append(f"static const glass_icon_variant_t variants_{kind}_{suffix}[] = {{")
            for name, size in variants:
                body.append(f"    {{{size}, {name}}},")
            body.append("};")
            entries.append((kind, suffix, len(variants)))
    body.append("const glass_icon_t glass_icons[] = {")
    for kind, suffix, count in entries:
        body.append(f'    {{"{kind}", "{suffix}", {count}, '
                    f'variants_{kind}_{suffix}}},')
    body.append("};")
    body.append(f"const size_t glass_icon_count = {len(entries)};")
    body.append("")
    decl.append("extern const glass_icon_t glass_icons[];")
    decl.append("extern const size_t glass_icon_count;")
    decl.append(f"// {total} bytes of baked icon coverage.")
    return "\n".join(decl) + "\n", "\n".join(body) + "\n", total


def emit_wmo() -> str:
    out = [
        "// Generated by tools/gen_assets.py - do not edit.",
        "// WMO weather interpretation codes (Open-Meteo) -> icon kind, sky",
        "// scene, short label, precipitation type, cloud-cover hint.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    int16_t code;",
        "    const char* icon;",
        "    const char* scene;",
        "    const char* label;",
        "    const char* precip;",
        "    uint8_t cloud_pct;",
        "} wmo_entry_t;",
        "",
        "static const wmo_entry_t kWmoTable[] = {",
    ]
    for code, icon, scene, label, precip, cloud in WMO_TABLE:
        cloud_val = cloud if cloud is not None else 255  # 255 = use API value
        out.append(f'    {{{code}, "{icon}", "{scene}", "{label}", "{precip}", '
                   f"{cloud_val}}},")
    out.append("};")
    out.append(f"static const int kWmoTableSize = {len(WMO_TABLE)};")
    return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(OUT), type=Path)
    parser.add_argument("--app-out", default=str(APP_OUT), type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    args.app_out.mkdir(parents=True, exist_ok=True)

    faces = [bake_face(*spec) for spec in FACES]
    font_decl, font_body, font_bytes = emit_font(faces)
    icon_decl, icon_body, icon_bytes = emit_icons()
    wmo_src = emit_wmo()

    header = "\n".join([
        "// Generated by tools/gen_assets.py - do not edit.",
        "// Baked font faces, weather icon masks, and the WMO lookup table.",
        "#pragma once",
        "",
        "#include \"glass_font.h\"",
        "#include \"glass_icons.h\"",
        "",
        font_decl,
        icon_decl,
    ])
    source = "\n".join([
        "// Generated by tools/gen_assets.py - do not edit.",
        "#include \"glass_assets.h\"",
        "",
        "#include <stddef.h>",
        "",
        font_body,
        icon_body,
    ])
    (args.out / "glass_assets.h").write_text(header)
    (args.out / "glass_assets.cc").write_text(source)
    (args.app_out / "wmo_table.h").write_text(wmo_src)

    print(f"glass_assets.h/.cc : {font_bytes:,} bytes of glyph coverage "
          f"({len(faces)} faces), {icon_bytes:,} bytes of icon coverage "
          f"({len(ICON_KINDS)} kinds x day/night)")
    print(f"wmo_table.h        : {len(WMO_TABLE)} codes -> {args.app_out}")


if __name__ == "__main__":
    main()