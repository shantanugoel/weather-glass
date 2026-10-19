# Attribution

## Zectrix board and panel drivers

`firmware/components/zectrix_board/` and `firmware/components/zectrix_epd/`
are taken from the Zectrix Note 4 reference projects (MIT). The board component
is **trimmed** in this repo: the ES8311 audio codec and ST25R391B NFC drivers
are removed, because a weather display drives neither, and that also removes the
`esp_codec_dev` managed dependency.

## Icon geometry

`vendor/zectrix-weather-glass/geometry.py` is vendored from
[itopinion/zectrix-weather-glass](https://zectrix.com/en/developer/projects/zectrix-weather-glass.html)
(MIT). `tools/gen_assets.py` rasterizes that vector geometry into the 8-bit
coverage masks in `firmware/components/glass_gfx/generated/glass_assets.cc`.
The `shapes()` table in the generator follows the reference renderer's shape
definitions, including its rounded-join trick for wide cubic strokes.

The card layout follows the same reference closely enough to be recognisable —
city and date across the top, one large observation panel, a slim metrics pill,
three forecast pills — but the geometry, ink selection and the whole sky layer
are written here.

## Inter font

`assets/Inter.ttf` is the Inter variable font, SIL Open Font License 1.1
(`assets/OFL-Inter.txt`). Only the codepoints and pixel sizes the card actually
draws are baked into the firmware, by `tools/gen_assets.py`.

## Astronomy and sky

The sun and Moon positions are computed from the standard low-precision
astronomical algorithms (mean elements plus the largest periodic terms), and
the day/night and star behaviour is keyed on the sun's geometric altitude with
the -0.83° sunrise/sunset convention. The cloud field is value-noise fBm; the
terminator in `draw_moon` is a cylinder-projection lit-fraction test. None of
it is vendored.

## Data

Weather comes from [Open-Meteo](https://open-meteo.com), which needs no API
key. Its non-commercial terms apply.
