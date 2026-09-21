# llm-tick

Real-time LLM token-usage tracker for the **Waveshare ESP32-S3-LCD-1.47B**
(ST7789 172×320, 8 MB PSRAM, 16 MB flash, native USB-C).

One small display that shows **your LLM usage live**:

- **Scene 0 — Usage** (default): session (5 h) + weekly (7 d) plan-limit bars,
  credits, and a per-model token split, pulled from `server.py` over the LAN.
- **Scene 1 — Weather standby**: when the numbers stop moving for 90 s the board
  drops to a WeatherAPI.com clock/standby screen; the instant tokens resume it
  jumps back to usage. Its background is a near-black canvas (Ferrari design
  language, `DESIGN-ferrari.md` — `#181818`, never pure black) carrying a
  subtle per-condition tint (day/night × clear/cloudy/rain/snow from the API's
  `is_day`), eased over ~1 s; white display type, gray body, one scarce
  Rosso-Corsa accent on the clock, which shows the board's local time
  (TZ `PST8PDT`).
- **Scenes 2…N — Generative art**: 8 effects (sand, plasma, rings, weave,
  Conway's life, cyclic CA, forest fire, Gray-Scott) at ~80 fps on the spare core.

A `PRESS` line over USB-serial cycles every scene — **this unit has no physical button** (the 1.47B wiki's BOOT/RESET do not exist on this build; the GPIO0 poll is inert). Onboard WS2812 shows the active scene colour.

## Merged from three open-source projects

| Piece | Source |
|-------|--------|
| `board.h`, `effects.*`, dual-core pipeline | [purzbeats/esp32-147b-genart](https://github.com/purzbeats/esp32-147b-genart) |
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

**No physical buttons:** the 1.47B wiki lists a RESET and BOOT button, but the
unit this was built against has neither — it behaves like the base 1.47. The
GPIO0 poll in `main.cpp` (`PIN_BTN`) is inert (nothing ever pulls it low), and
flashing needs no button-hold (onboard auto-download circuit). The `PRESS`
serial line is the only scene control.

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
~5 % luminance wobble is imperceptible. Effects intentionally keep full-bleed
art (a shimmer inside artwork is acceptable; the complaint was the text).
If it is ever still visible, the remaining cures are physical: a shroud over
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
    effects.h/.cpp   8 genart effects + palettes (from genart)
    ui.cpp           usage + weather scene rendering
    data.cpp         wifi, mDNS, JSON poll, idle->standby
    weather_api.*    WeatherAPI.com client
  platformio.ini     build config (native USB, PSRAM, huge_app)
  secrets.h.example  copy to src/secrets.h (gitignored)
server.py            LAN usage server (source-agnostic)
server_test.py       one check for the server
```
