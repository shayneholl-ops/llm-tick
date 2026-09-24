# llm-tick

Real-time LLM token-usage tracker for the **Waveshare ESP32-S3-LCD-1.47B**
(ST7789 172×320, 8 MB PSRAM, 16 MB flash, native USB-C).

One small display that shows **your LLM usage live**:

- **Scene 0 — Usage** (default): session (5 h) + weekly (7 d) plan-limit bars,
  credits, and a per-model token split, pulled from `server.py` over the LAN.
- **Scene 1 — Weather standby**: when the numbers stop moving for 90 s the
  board drops to a WeatherAPI.com clock/standby screen; the instant tokens
  resume it jumps back to usage. Its background is an animated vector scene
  ported 1:1 (2026-09-22) from the in-repo design export
  (`stitch_animated_lvgl_weather_backgrounds/`, `src/wxscene.cpp`): **by day** a
  North Shore coast — three-stop sky, sun glow, drifting mist, the Lions and
  Grouse silhouettes with snow-crested peaks, swaying conifers, animated sea
  swells and a Lions Gate bridge line; **by night** a Burrard Inlet nocturne —
  undulating aurora ribbons, twinkling stars, a crescent moon, the bridge with
  a beacon and water reflections. The condition drives the particles: rain
  drizzle, snow flurry, storm gusts, clear-day shimmer — all on the design's
  "calmed, subtle & slower" cadence (8–18 s loops, eased opacity, no snap).
  All procedural, drawn straight into the framebuffer; white display type,
  gray body, one scarce red accent on the clock, which shows the board's local
  time (TZ `PST8PDT`). Beside the temperature sits a **procedural animated
  condition icon** (sun / moon / cloud / rain / snow / fog / storm) — drawn
  from primitives, so there are no image assets and no LVGL: sun rays breathe,
  clouds drift, rain streaks and snow flakes fall, the moon's halo pulses, a
  storm flashes locally inside the icon. Monochrome (white ink / gray puffs /
  muted detail), and it shrinks when the reading is wide.

A `PRESS` line over USB-serial cycles the scenes. The unit does have a RESET and a BOOT button (see "Buttons" below), but on this build neither is a scene control — `PRESS` is. Onboard WS2812 shows the active scene colour.

## Merged from three open-source projects

| Piece | Source |
|-------|--------|
| `board.h`, dual-core render pipeline | [purzbeats/esp32-147b-genart](https://github.com/purzbeats/esp32-147b-genart) (its 8 genart effects were removed 2026-09-21) |
| `server.py`, mDNS + JSON poll, bar UI, idle logic | [polo7261/esp32-claude-usage](https://github.com/polo7261/esp32-claude-usage) |
| `weather_api.*` (WeatherAPI.com) | [icefox0801/ESP32-S3-LCD-1.47-Tiny-Board](https://github.com/icefox0801/ESP32-S3-LCD-1.47-Tiny-Board) |

## Hardware truth (1.47B)

Panel/SPI pins match the base 1.47. **The backlight pin differs per unit:** the
1.47B schematic says GPIO46, but the unit this was built against lights the
backlight only on **GPIO48** — verified on hardware with a blink matrix (46 and
47 do nothing). If your panel stays dark, blink-test 46/48 before chasing
anything else and set `PIN_BL` in `src/board.h`. Drive it HIGH after
`lcd.init()` — it defaults off via a 10K gate pulldown. Native USB-Serial JTAG:
`VID 0x303A / PID 0x1001`.

**Backlight color (per unit):** the unit this was built against has a
**single cool-cast (blue/violet-rich) backlight LED** — GPIO48 drives it and
GPIO46/47 do nothing in either polarity (an 8-state polarity matrix, the `BLX`
serial command, proved there is no hidden white channel on this build).
All-black content still glows periwinkle, and camera photos render the cast
pink. The firmware compensates in software: `wb565()` in `src/tick.h`
pre-shifts every UI/effect color toward green (G×14/16, B×10/16 —
camera-calibrated with the `WB 7` split field, which locks the camera's
white-balance on a white half of the screen so the dark half's true residual
cast is measurable), so neutral content emits neutral through the cast. If
your unit's backlight is white, set both gains to 16/16.

**Buttons:** the unit does have a RESET and a BOOT button (the 1.47B wiki is
right; the "no button" note in earlier versions of this doc was wrong), but on
this build neither is a scene control: a RESET press does a **full reboot** —
the ROM reports `rst:0x15 (USB_UART_CHIP_RESET)`, i.e. it is wired through the
USB-UART chip's reset, not EN — and just returns the display to the usage
scene; no BOOT press ever produced a scene change (90 s of serial watching),
so the GPIO0 poll in `main.cpp` (`PIN_BTN`) stays inert. Flashing needs no
button-hold (onboard auto-download circuit). The `PRESS` serial line is the
scene control.

**Panel geometry:** the 172-wide glass is centered in the ST7789's 240-wide RAM
(`offset_x 34`), not at origin as some references claim — on the unit this was
built against, a full `172x320 @0,0` push left a ~20%-wide stale strip at the
glass edge, while `240x320 @34,0` covers it edge-to-edge (verified with
on-board camera fill/half-split tests). If you see a stale strip on one edge,
that is the symptom: check `offset_x` in `src/board.h`.

**Top-of-glass "noise" (re-investigated 2026-09-19, root cause found):**
the band is **panel-side**: this ST7789's gate-array end (the last ~60 rows
of display RAM) **meanders in luminance** — a stochastic ±5 % modulation
(broadband plus a slow ~25 s drift; a 60 s autocorrelation sweep shows no
periodic beat), visible only with the backlight on. Proven panel-side by
dark-room 4 fps captures: the band region wobbles Δ≈1.7–2.8 Y while every
other region reads ≤0.1; the wobble vanishes at backlight 0 and scales
~linearly with backlight duty (5.2/4.5/4.4 % of level at 100/50/25 %);
it is content- and angle-independent; and it **follows RAM-row identity,
not the physical glass** — remove the 180° flip and the band moves to the
opposite edge. On this unit the 180° mount (USB end up) puts that RAM end
at the viewing top edge, so it is the "top band"; the unflipped reference
machine parks it at the bottom edge, unnoticed. The 25/60 fps analyses of
2026-09-18 undersampled it (its energy sits at ~0.04–2 Hz; 4 fps resolves
it, 25 fps aliases most of it away).

**Fix:** the UI scenes keep **rows 0–59 pure background** (`ui.cpp` — title
at y 64, divider at 94, bars at 104/176/248, weather number at 72), so the
meander modulates only a uniform field — no edges in the band means the
~5 % luminance wobble is imperceptible. If it is ever still visible (the user
can still see a slow breathing on the weather background, measured 4.4 Y
peak-to-peak in the band at 4 fps while the desk control reads 0.25), the
remaining cures are physical: a shroud over
the top ~7 mm, or rotating the module 180° (then remove `offset_rotation 2`)
to park the band at the bottom edge.

**2026-09-19 — wifi stress-crash, mitigated:** a 15-min stress loop
(scene switch every 6 s ≈ 10 HTTP fetches/min, on a congested AP — the Pi
streams 1080p RTSP all night) crash-looped **every** firmware, including
the pristine 2026-09-18 build: `wifi:ebuf_free: invalid type` + TLSF heap
asserts in the wifi RX path — the driver's *dynamic* RX ebuf pool (malloc'd
per frame burst from the internal-RAM heap) corrupting heap metadata under
bursty load. With the radio disabled the same loop ran 15 min clean; normal
60-s polling runs clean too, which is why real use never hit it. The ARDUINO
build's driver is prebuilt (its buffer pools can't be retuned per project),
so the shipped mitigation is behavioral: on-switch refreshes are gated to
once per 30 s (`data.cpp`), capping even a mashed `PRESS` line at ~2 fetches/
min — the old 6-s hammer now runs clean. If wifi oddities ever return,
power-cycling the router is the complementary cure.

## Build & flash

```bash
cd llm-tick
cp secrets.h.example secrets.h     # fill in WiFi + server + weather key
pio run                            # build
pio run -t upload --upload-port COMx   # flash (auto-download circuit; no button to hold)
```

## Server (the data source)

`server.py` serves usage JSON on port 8266. It is source-agnostic — point it at
any LLM usage feed:

```bash
# local llama.cpp (default; token log at ~/.llama.cpp/usage.jsonl)
python server.py

# Claude Code (ccusage) — adds a per-model token-split page
CCUSAGE_BIN=ccusage python server.py

# bind to the LAN so the board can reach it (default is loopback)
BIND_HOST=192.168.1.50 python server.py
```

The board finds the server by **mDNS name** (`SERVER_HOST` in `secrets.h`, no
reflash when the LAN renumbers); the static IP is a fallback.

### Feeding it from a local llama-server

`llama-server` writes no usage log, but started with `--metrics` it exposes
cumulative counters (`llamacpp:prompt_tokens_total`,
`llamacpp:tokens_predicted_total`). `llama_usage_poller.py` samples them every
30 s, diffs them, and appends rows in exactly the format `server.py` reads
(`{"ts":…,"model":…,"input_tokens":…,"output_tokens":…}`):

```bash
# on any machine that can reach the llama-server
MODEL_NAME=Qwen3-27B LLAMA_BASE=http://192.168.1.94:12345 \
  OUT_PATH=/path/to/usage.jsonl python llama_usage_poller.py

# then point the server at that log
LOG_PATH=/path/to/usage.jsonl BIND_HOST=192.168.1.50 python server.py
```

### Showing the GPU's live load and temperature

The usage page has a GPU row (load %, edge temp, and hotspot when the card
reports it). `server.py` fills it by sampling `rocm-smi` **on the model host
over SSH** — most such hosts expose no HTTP telemetry, and llama-server's own
`/metrics` carries no GPU counters. Configure it with:

```bash
# default: admin-a8@192.168.1.94, card0
GPU_SSH=admin-a8@192.168.1.94 GPU_CARD=card0 python server.py

# switch the row off entirely
GPU_DISABLE=1 python server.py
```

Authentication is key-based (`BatchMode=yes`), so nothing prompts and no
password is stored; install a key on the host first
(`ssh-copy-id admin-a8@192.168.1.94`). Sampling is cached (5 s when healthy,
30 s after a failure) and every error is swallowed, so an unreachable host
degrades that one row to `--` instead of breaking the usage payload. Pick
`GPU_CARD` from the keys of `rocm-smi --showuse --showtemp --json` (`card0`,
`card1`, …) — on a box with an iGPU, the discrete card is normally `card0`.

Because the row is a live gauge, the board polls every **15 s while the usage
page is displayed** (60 s elsewhere, 10 s after a failure), which is the
cadence the wifi RX path has been stable at.

Counters reset when llama-server restarts; the poller treats a decrease as a new
baseline, so no negative rows — but usage during the restart gap, and any usage
while the poller is stopped, is not recorded. Note the bar semantics `server.py`
uses: the **session** bar is 5 h tokens against a 2 M budget, and the **weekly**
bar is the busiest model's *share* of the last 24 h (with a single model that
reads 100 % whenever there is any usage) — both are one-line tweaks in
`server.py`.

## Self-check

`server.py` has one runnable check:

```bash
python -m pytest -q server_test.py     # or: python server_test.py
```

## Layout

```
llm-tick/            firmware (PlatformIO, Arduino)
  src/
    main.cpp         pipeline: dual-core render + scene dispatch + serial cmds
    tick.h           shared state (Usage struct, scene table)
    board.h          verified 1.47B pins + LGFX panel config
    ui.cpp           usage + weather scene rendering
    data.cpp         wifi, mDNS, JSON poll, idle->standby
    weather_api.*    WeatherAPI.com client
  platformio.ini     build config (native USB, PSRAM, huge_app)
  secrets.h.example  copy to src/secrets.h (gitignored)
server.py            LAN usage server (source-agnostic)
llama_usage_poller.py  llama-server /metrics -> usage log (feeds server.py)
server_test.py       one check for the server
```
