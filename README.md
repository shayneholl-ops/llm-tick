# llm-tick

Real-time LLM token-usage tracker for the **Waveshare ESP32-S3-LCD-1.47B**
(ST7789 172×320, 8 MB PSRAM, 16 MB flash, native USB-C).

One small display that shows **your LLM usage live**:

- **Scene 0 — Usage** (default): session (5 h) + weekly (7 d) plan-limit bars,
  credits, and a per-model token split, pulled from `server.py` over the LAN.
- **Scene 1 — Weather standby**: when the numbers stop moving for 90 s the board
  drops to a WeatherAPI.com clock/standby screen; the instant tokens resume it
  jumps back to usage.
- **Scenes 2…N — Generative art**: 8 effects (sand, plasma, rings, weave,
  Conway's life, cyclic CA, forest fire, Gray-Scott) at ~80 fps on the spare core.

**BOOT** button cycles every scene. Onboard WS2812 shows the active scene colour.

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

**Panel geometry:** the 172-wide glass is centered in the ST7789's 240-wide RAM
(`offset_x 34`), not at origin as some references claim — on the unit this was
built against, a full `172x320 @0,0` push left a ~20%-wide stale strip at the
glass edge, while `240x320 @34,0` covers it edge-to-edge (verified with
on-board camera fill/half-split tests). If you see a stale strip on one edge,
that is the symptom: check `offset_x` in `src/board.h`.

**Top-of-glass "noise" (investigated 2026-09-18, closed as optical, not
electrical):** a hazy/flickering band was reported at the top of the glass.
Every panel-side measure came back clean:
- 750 frames @25 fps, whole-panel mean luminance constant (zero flicker).
- 60 fps native capture (300 frames, 5 s): lag-1 autocorrelation of the level
  series is 0.97 in every region — no mains/PWM-frequency flicker resolvable;
  the band's total 5 s variation is 0.17 %, below the perceptual flicker
  threshold.
- Per-scene sweep (all 10 scenes @25 fps): text always crisp — no random
  pixel corruption; SPI 80 MHz matches the proven-stable reference machine.
- Region controls, same clip: only the glass top band varied (frame-delta
  0.55, slow ±5 drift over 30 s); power button LED (0.016), glass middle
  (0.011), USB-port area (0.002) all rock-stable.
- Backlight is plain DC HIGH (Waveshare's demo uses 90 % PWM; DC can't PWM-flicker).
- Board serial healthy: stable render cadence, no errors, NTP + weather + usage all live.

Conclusion: the only reproducible signal is a *slow* luminance meander
(±4–5 %, no fast flicker) confined to the glass's reflection zone — a
2 h on-unit sentinel reproduced it in 19/19 idle cycles. Consistent with
ambient light (dimmable/breathing bulb, PC case glow) reflecting off the
glass at the viewing angle, perceived as noise.
If it ever persists at one viewing angle in a dark room (no varying light
source in the reflection path), re-investigate the panel: grab a phone
close-up video of the band and re-run the region-delta analysis.

## Build & flash

```bash
cd llm-tick
cp secrets.h.example secrets.h     # fill in WiFi + server + weather key
pio run                            # build
pio run -t upload --upload-port COMx   # flash (hold BOOT to enter download mode)
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
    main.cpp         pipeline: dual-core render + scene dispatch + button
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
