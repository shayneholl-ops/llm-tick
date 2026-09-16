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

Panel/SPI pins match the base 1.47, but **backlight is GPIO46** (base board
uses 48). Drive `PIN_BL` HIGH after init — it defaults off. Native USB-Serial
JTAG: `VID 0x303A / PID 0x1001`.

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
  llm-tick.ino       pipeline: dual-core render + scene dispatch + button
  tick.h            shared state (Usage struct, scene table)
  board.h           verified 1.47B pins + LGFX panel config
  effects.h/.cpp    8 genart effects + palettes (from genart)
  ui.cpp            usage + weather scene rendering
  data.cpp          wifi, mDNS, JSON poll, idle->standby
  weather_api.*     WeatherAPI.com client
  secrets.h.example copy to secrets.h (gitignored)
server.py            LAN usage server (source-agnostic)
server_test.py       one check for the server
```
