# llm-tick handoff (2026-09-19)

Portable state for picking up this project in a fresh session. Read this first, then `README.md` (Hardware truth section) and `src/board.h` comments.

## What this is
A Waveshare **ESP32-S3-LCD-1.47B** (ST7789 172×320, 8 MB PSRAM, 16 MB flash) turned into a live **LLM-usage display**: polls the user's `server.py` (port 8266, mDNS `GMKK12`) every 60 s, shows session/weekly usage, auto-switches to a **weather standby** scene (WeatherAPI.com, Vancouver, background follows the actual day/night + condition) after 90 s of no data change, plus 8 genart effects cycled by a `PRESS` line over USB-serial — **this unit has no physical button** (the 1.47B wiki's BOOT/RESET buttons do not exist on this build; the GPIO0 poll in `checkButton()` is inert).

## Locations
| Thing | Path / value |
|---|---|
| Canonical repo (master @ `93ce45b` pushed 2026-09-19: flicker fix + wifi gate) | `C:\Users\hxp-n\llm-tick-repo\` |
| Debug toolkit (diag project + tools + evidence) | `C:\Users\hxp-n\llm-tick-diag\` (wxprobe, sweep, bandwatch, matrix, beat, bandmap, crashloop, Waveshare demo, capture clips, logs) |
| Stale pre-push sandbox copy (no .git, reference only) | `C:\Users\hxp-n\llm-tick\` (contains `ref-genart\`) |
| Sample feed data | `C:\Users\hxp-n\llm-tick-test\` |
| Board port | **COM4** (USB-Serial/JTAG), flash: `pio run -d C:\Users\hxp-n\llm-tick-repo -t upload --upload-port COM4` (~30 s) |
| pio / python | `C:\Users\hxp-n\AppData\Local\hermes\hermes-agent\venv\Scripts\{pio.exe,python.exe}` |
| Camera (the only "eye") | OctoStream RTSP `rtsp://192.168.1.86:554/stream`, 60 fps source; grab: `ffmpeg -rtsp_transport tcp - <url> -frames:v 1 -update 1 -y out.jpg`. **Service is flaky** (OctoPrint on the Pi; if 554 is refused, user restarts it on the Pi). |
| Secrets | `src/secrets.h` (gitignored): WiFi `Coromandel`, weather key filled, server 192.168.1.69:8266. **Never echo or commit.** Pushes use `gh` credentials; if a push fails on auth, re-run `gh auth setup-git`. |

## State: flicker root-caused + fixed; crash found + fixed (committed 2026-09-19)
- `0a6abb0` — three **per-unit hardware facts** (backlight GPIO48; glass centered `offset_x 34`; glass mounted **180°** → `offset_rotation 2`).
- `4de0fbb` — weather key live; serial `PRESS` line emulates BOOT (hands-free scene testing) — in fact the **only** scene control (no physical button on this unit).
- `c621bed`+`e79029a`+`d248a6e` — 2026-09-18 top-band investigation, *then* closed as optical — **now superseded** (below).
- **2026-09-19 (this session)**:
  1. **Flicker root cause (proven in the dark with the camera at 4 fps):** the ST7789's gate-array **end — the last ~60 RAM rows — meanders in luminance** (stochastic ±5 % of local level, broadband + ~25 s drift; NOT a periodic beat — 60 s autocorrelation is flat, peak 0.23). It **requires backlight on** (vanishes at BL 0), scales ~linearly with backlight duty (5.2/4.5/4.4 % at 100/50/25 % duty), is **content-independent**, is angle-independent, and **follows RAM-row identity, not physical glass**: with the flip removed the band moves to the opposite edge (bandBot Δ2.26 / bandTop Δ0.09 unflipped ↔ bandTop Δ1.65–2.8 flipped). This unit's 180° mount puts RAM-row-319-at-edge at the user's **top** edge → the perceived "flickering band". The 25/60 fps cameras of 2026-09-18 undersampled it (the meander's energy is at ~0.04–2 Hz; 4 fps resolves it, 25 fps partially aliases it away). The reference machine (no flip) parks the same meander at the bottom edge, unnoticed.
  2. **Flicker fix (shipped):** keep display **rows 0–59 pure background** in both UI scenes (ui.cpp layout shift: title y 8→64, divider 38→94, bars 52/128/204→104/176/248, tokens info row 42→96, status h-24→h-16, weather big number 60→72). A ±5 % meander on a uniform field has no edges to modulate → imperceptible. (The earlier 1-row mirror guard — `memcpy` row 1→row 0 — does NOT fix it; the band is ~60 rows, and the meander modulates the row's own luminance regardless of content. Guard kept anyway as a row-319 nicety; it is harmless.)
  3. **Dynamic weather background (user request):** the standby scene's background now follows the real conditions — WeatherAPI `current.is_day` × condition code (clear/cloudy/rain/snow) → 8-color palette (day sky-blue/light-gray/slate/snow-white; night navy/dark-gray/blue-gray/slate) — and **eases ~1 s** between colors (per-frame 1/10 lerp in `ui.cpp`); text colors flip dark/light with background luminance. Also fixed the clock, which showed UTC (`gmtime`) despite the board's `PST8PDT` TZ — now `localtime`. On-screen hint now says `PRESS: cycle scenes`.
  4. **Crash found + mitigated (pre-existing, not from this session's code):** under a bursty-load stress loop the board crash-looped ~every 24–48 s: `wifi:ebuf_free: invalid type` + `assert block_locate_free` / `multi_heap_free (head != NULL)` — the wifi driver's **dynamic RX ebuf pool (32 × ~1.7 KB, malloc'd per frame burst)** corrupting TLSF heap metadata (detected in `wDev_ProcessRxSucData → wifi_malloc`). Proven by 15-min loops (`crashloop.py <seconds> <pressEvery>`): 6-s PRESS hammer = RED on **every** build (full, −blTask, −guard, **pristine d248a6e control** — my code exonerated); **radio disabled = 0 crashes/15 min GREEN** → the trigger is WiFi RX load (the Pi streams 1080p RTSP all night; today's AP is busier than 2026-09-18's). Normal 60-s polling = **0 crashes/15 min GREEN** (why the board never crashed in real use).
  - **Shipped mitigation:** `tickLogic` now gates the on-switch refreshes to **once per 30 s** (usage + weather), so even a mashed `PRESS` line can't burst the driver past ~2 fetches/min. Verified: the same 6-s hammer that crash-looped every build before now runs clean. NOTE: a `sdkconfig.defaults` with fully-static RX buffers was tried first — **the ARDUINO build ignores it** (prebuilt driver SDK; only the pure-ESP-IDF build path honors sdkconfig), so the file was dropped; the driver's pool size is not tunable per-project in this framework. If crashes ever return, the cures are: power-cycle the router (AP-side), or rebuild the app on the espidf framework where `STATIC_RX_BUFFER_NUM=32`/`DYNAMIC_RX_BUFFER_NUM=0` actually compile in.
  - Bisect table (15-min loops, 2026-09-19): full-diag+layout @6-s-hammer **crash ~24 s/boot**; −blTask **~24 s/boot**; −guard **~45 s/boot**; d248a6e control **~45 s/boot**; d248a6e+NO_WIFI **0 crashes**; full build @60-s-normal **0 crashes**; final build @6-s-hammer **see commit (expected green)**.
- Board is running live in the user's PC (USB end up); WS2812 ring shows scene color (working — don't "fix" without an on-unit test).
- Serial commands live in the build (`PRESS` = the scene cycler / `LED 0|1` / `BL 0|25|50|100` / `PAT 0..6` / `ST`) — harmless at runtime (BL 100 = DC, PAT 0 = normal), kept on purpose; 200-Hz-class backlight is a 10-ms-tick 5-substep task (`blTask`), **not** delayMicroseconds (that froze the board) — do not reintroduce spins.

## Camera measurement facts (use these, not the 25/60 fps recipes, for panel light)
- 1080×1920 RTSP regions: bandTop (330,515,230,55) = display rows ~12–37; bandMid (320,840,280,60); bandBot (340,1130,240,50); caseCtrl (100,100,400,80) = ambient control; pwrBtn (150,350,130,110).
- **Measure the meander at 4 fps** (matrix.py, 48 frames = 12 s): a real meander shows Δ≈1.7–2.8 in the band vs ≤0.1 elsewhere; a clean region reads ≤0.1 at 4 fps. In the dark, compare **deltas and within-run ratios, never absolute Y** (camera auto-exposure drifts run-to-run; caseCtrl is the control). pwrBtn shows a separate ~2 % case power-LED breathing — ignore it.
- 10 fps × 60 s (`beat.py`) resolves the meander's spectrum; 10 fps is the floor for seeing ~1–2 Hz structure.

## Open (user-side, 30 s each)
1. **Eyes on the band:** the layout fix makes the meander zone edge-free — ask the user to glance at the top edge in the dark. If they still perceive flicker, the fallbacks are (a) a small black shroud over the top ~7 mm of glass (the band spans ~60 of 320 rows ≈ 6–7 mm), or (b) reroute the USB cable + rotate the module 180° (then remove `offset_rotation 2`): the band moves to the bottom edge, like the reference machine.
2. **The AP:** the stress-crash was triggered by bursty WiFi RX load (the Pi streams 1080p RTSP to the session all night; today's AP is busier than on 2026-09-18). The 30-s refresh gate (shipped) stops scene-mashing from bursting the driver, and normal 60-s polling was already clean — but if any wifi oddity ever returns, power-cycling the router is the complementary cure.

## Gotchas
- **Per-unit variance is real**: another 1.47-family machine (genart reference, COM3) runs BL 46 / `@34` / 320×172 fine. Trust on-unit camera + serial evidence over any schematic.
- ST7789 has **no readback**; `lcd.init()` returns true even with a wrong pinout — hardware evidence wins.
- If the panel goes dark again: suspect **USB power delivery** (backlight draw) first.
- `LGFX_Sprite` has no plain `fill(color)` — full fill is `fillRect(0,0,w,h,color)`.
- Standby needs 90 s of **no data change**; a live feed that moves will keep the board on the usage scene.
- Camera moves between runs — re-derive framing each time; the board sits in the upper camera area, USB end up.
- `sendcmd.ps1` args: PowerShell quoting eats arrays — one simple quoted string per command (`-Cmds "PRESS"`), or the board receives one mashed line and ignores it.
- **crashloop.py owns COM4** — don't run sendcmd.ps1 while a loop is running (exclusive port).
