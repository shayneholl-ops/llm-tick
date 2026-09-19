# llm-tick handoff (2026-09-18)

Portable state for picking up this project in a fresh session. Read this first, then `README.md` (Hardware truth section) and `src/board.h` comments.

## What this is
A Waveshare **ESP32-S3-LCD-1.47B** (ST7789 172×320, 8 MB PSRAM, 16 MB flash) turned into a live **LLM-usage display**: polls the user's `server.py` (port 8266, mDNS `GMKK12`) every 60 s, shows session/weekly usage, auto-switches to a **weather standby** scene (WeatherAPI.com, Vancouver) after 90 s of no data change, plus 8 genart effects cycled by the BOOT button.

## Locations
| Thing | Path / value |
|---|---|
| Canonical repo (pushed, master @ `d248a6e`) | `C:\Users\hxp-n\llm-tick-repo\` |
| Debug toolkit (diag project + tools + evidence) | `C:\Users\hxp-n\llm-tick-diag\` (wxprobe, sweep, bandwatch, rtspwatch, Waveshare demo, 24 capture clips, logs) |
| Stale pre-push sandbox copy (no .git, reference only) | `C:\Users\hxp-n\llm-tick\` (contains `ref-genart\`) |
| Sample feed data | `C:\Users\hxp-n\llm-tick-test\` |
| Board port | **COM4** (USB-Serial/JTAG), flash: `pio run -d C:\Users\hxp-n\llm-tick-repo -t upload --upload-port COM4` (~30 s) |
| pio / python | `C:\Users\hxp-n\AppData\Local\hermes\hermes-agent\venv\Scripts\{pio.exe,python.exe}` |
| Camera (the only "eye") | OctoStream RTSP `rtsp://192.168.1.86:554/stream`, 60 fps source; grab: `ffmpeg -rtsp_transport tcp -i <url> -frames:v 1 -update 1 -y out.jpg`. **Service is flaky** (OctoPrint on the Pi goes up/down; if 554 is refused, the Pi is up but the service is down — user must restart it on the Pi). |
| Secrets | `src/secrets.h` (gitignored): WiFi `Coromandel`, weather key filled, server 192.168.1.69:8266. **Never echo or commit.** Pushes use `gh` credentials; if a push fails on auth, re-run `gh auth setup-git`. |

## State: everything works, all pushed
- `0a6abb0` — three **per-unit hardware facts** (the 1.47B schematic does not hold on this unit; on-unit camera evidence wins):
  1. Backlight = **GPIO48** (schematic says 46; blink-matrix verified).
  2. Glass **centered** in ST7789's 240-wide RAM → `memory 240x320, offset_x 34, offset_y 0` (`@0,0` leaves a ~20 %-wide stale strip).
  3. Glass mounted **180°** on this unit → `offset_rotation 2` (tied to the user's USB-end-up mount; if they flip the module, remove the flip).
- `4de0fbb` — weather key live (standby shows real Vancouver data); serial `PRESS` line emulates BOOT (hands-free scene testing); wx fetch logs ok/FAIL + temp.
- `c621bed` + `e79029a` + `d248a6e` — the **top-band "noise" investigation**, closed as **optical, not electrical** (see below).
- Board is running live in the user's PC (USB end up, their viewing orientation); WS2812 ring shows scene color (working — don't "fix" without an on-unit test).

## The one open thread (user-side test)
User perceived a hazy/"electronic-noise" band at the **top of the glass**. Full evidence in README ("Top-of-glass noise" section): 750-frame @25 fps + 300-frame @60 fps luminance analysis (zero flicker; lag-1 ≈ 0.97 everywhere), all-10-scene sweep (no pixel corruption; text always crisp), same-clip region controls (only the reflection zone drifts; power button/glass-mid/USB area rock-stable), 2 h sentinel (19/19 idle cycles reproduce a ±4–5 % *slow* meander), DC backlight, healthy serial. Conclusion: slowly-varying ambient light reflecting off the glass at the viewing angle.

**Confirming test (30 s, only the user can do it):** view the board dead-on + move head — if the haze moves/changes shape it's reflection (case closed); or dim/cover lights above the module. **If it persists at one fixed angle in a dark room** → phone close-up video of the band (send to agent) → reopen panel-side investigation (re-run the region-delta analysis on the close-up).

## Gotchas
- **Per-unit variance is real**: another 1.47-family machine (genart reference, COM3) runs BL 46 / `@34` / 320×172 fine. Trust on-unit camera + serial evidence over any schematic; both are documented in `board.h`/README.
- ST7789 has **no readback**; `lcd.init()` returns true even with a wrong pinout — hardware evidence wins.
- If the panel goes dark again: suspect **USB power delivery** (backlight draw) first.
- `LGFX_Sprite` has no plain `fill(color)` — full fill is `fillRect(0,0,w,h,color)`.
- Standby needs 90 s of **no data change**; a live feed that moves will keep the board on the usage scene (not a bug).
- Camera moves between runs — re-derive framing each time; the board sits upper area of the camera frame, USB end up.
- Weather scene shows "n/a" weather only if the key is emptied again; otherwise hourly refresh is automatic.
