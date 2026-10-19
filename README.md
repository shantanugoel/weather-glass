# Weather Glass — Zectrix Note 4

A weather display for the [Zectrix Note 4](https://zectrix.com) (ESP32-S3,
4.2" 400×300 e-paper). The background is a sky synthesised from your real
coordinates and the real time — sun and moon position, star field, cloud mass,
precipitation, fog, hills — and the forecast sits on top of it in frosted
"liquid glass" panels.

The ESP32-S3 is in deep sleep between refreshes and the panel is unpowered:
e-paper holds an image without power, so the only thing costing energy is the
radio during a fetch.

## How it works

```
RTC alarm ──▶ wake ──▶ Wi-Fi ──▶ Open-Meteo ──▶ parse ──▶ draw ──▶ 16-level panel ──▶ sleep
              ▲                                        │
              └──────── EXT1 on PCF8563 INT ◀──────────┘
```

One wake does one fetch, one render, one refresh. Cadence is 20 minutes by day
and 2 hours at night (`weather_next_fetch_delay_sec`), and a failed fetch
retries in 5 minutes rather than freezing the panel.

There is no NTP client: Open-Meteo is asked with `timezone=auto`, so the
response carries its own UTC offset and a local timestamp, and the PCF8563 is
set from the weather fetch that was happening anyway.

### The sky is computed, not downloaded

`glass_sky.cc` builds the background from the WMO code, cloud cover, humidity,
wind and precipitation:

* sun and moon placed by real ephemeris (`glass_astro.cc`) for your latitude,
  longitude and the current instant — not picked from a lookup table,
* fBm cloud field, thresholded by cloud cover and lit from the sun's position,
* stars weighted by how far below the horizon the sun is,
* precipitation tilted by the reported wind direction,
* two hill ridges along the bottom, so the glass has something to frost over.

Deterministic per hour (`glass_sky_seed`), so a re-render of the same hour
agrees with itself and the next fetch gives a different sky.

### Shared compositor

`firmware/components/glass_gfx` and `glass_app` are plain C++ with no ESP-IDF
dependency. The host simulator builds the *same* sources and writes PNGs, so
the layout is reviewed before anything is flashed:

```sh
cd sim && make run              # every scenario → sim/out/*.png
./out/glass-sim out rain        # one scenario
./out/glass-sim out --skies     # the background alone, no card
./out/glass-sim out --astro     # astronomy sanity table
```

The simulator also **fails the build of a frame**: `glass_diag_*` reports a
missing glyph, an icon kind that fell back to the question mark, or a string
wider than the pill it was drawn in. Those are the bugs a PNG review is bad at.

To look at the images cheaply:

```sh
.venv/bin/python tools/preview.py sheet sim/out --cols 5 --scale 0.46
.venv/bin/python tools/preview.py zoom rain sim/out --region card
```

## Build and flash

Everything you configure lives in `config.toml` at the repository root —
Wi-Fi, coordinates, city, refresh cadence. It is gitignored because it holds
the passphrase:

```sh
cp config.example.toml config.toml   # then edit it

tools/idf.sh build
tools/idf.sh -p /dev/ttyACM0 flash monitor
```

`tools/idf.sh build` regenerates `firmware/sdkconfig.defaults.local` from
`config.toml` (via `tools/gen_config.py`) as part of CMake configure, so the
TOML is the only file to touch — and a bad value fails the build with a line
naming the key, rather than flashing a device that cannot reach the network.
`GLASS_SSID=… GLASS_PASSWORD=…` override on the command line for a bench test.

`tools/idf.sh` wraps this machine's ESP-IDF v6.1 install (see the comment at
the top — the stock `export.sh` does not work with the IDF Installation
Manager layout).

Assets are generated, not checked in by hand:

```sh
.venv/bin/python tools/gen_assets.py     # fonts, icons, WMO table
cd sim && make assets                    # same thing, from the sim
```

## Hardware notes

* **Only GPIO0–21 are RTC IOs on the ESP32-S3.** The Note 4's Up button is
  GPIO39, so it cannot wake the board from deep sleep; OK (GPIO0) and Down
  (GPIO18) can, along with the PCF8563 INT line on GPIO5. Waking is therefore
  alarm-driven, with buttons as a convenience rather than the mechanism.
* The panel is an SSD2683 driven at 4 bpp (16 levels). Everything is composited
  at 8 bits and quantised once at the end, with a per-pixel dither weight: the
  sky wants dithering or its gradients band, text and glass rims want none or
  they go fuzzy.
* The compositor holds five 400×300 planes, so PSRAM is mandatory
  (`CONFIG_SPIRAM=y`), not an optimisation.
* The board component is trimmed from the Zectrix reference: no audio codec and
  no NFC, which also drops the `esp_codec_dev` dependency.

## Layout of the repo

```
firmware/components/glass_gfx/   canvas, font, astronomy, sky, glass, card
firmware/components/glass_app/   Open-Meteo parse, WMO table, formatting, cadence
firmware/components/zectrix_*    vendored board + SSD2683 driver
firmware/main/                   wake, Wi-Fi, HTTPS, panel push, deep sleep
sim/                             host build of the same compositor → PNGs
tools/                           asset baker, config baker, preview, IDF wrapper
vendor/                          MIT-licensed reference geometry (see docs/ATTRIBUTION.md)
```

Check the network path without flashing — parse a real Open-Meteo response
through the same parser the device runs:

```sh
curl -s "https://api.open-meteo.com/v1/forecast?latitude=12.9698&longitude=77.7499" \
     "&current=temperature_2m,weather_code&daily=weather_code&forecast_days=3" -o /tmp/om.json
(cd sim && make && ./out/glass-sim out --json /tmp/om.json)
```

## License

Original code MIT. Fonts, icons and the vendored geometry carry their own
terms — see `docs/ATTRIBUTION.md`.
