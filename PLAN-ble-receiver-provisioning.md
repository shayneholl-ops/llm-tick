# Implementation Plan — Keychron BLE→USB receiver + Web WIFI/BLE provisioning

Target: **llm-tick** firmware on the Waveshare **ESP32-S3-LCD-1.47B** (ST7789 172×320, 8 MB PSRAM,
16 MB flash, native USB-C, PlatformIO/Arduino, `espressif32@7.0.1`).
Research backing: `.scratch/research/feature1-ble-keyboard-usb-bridge.md` (F1) and
`.scratch/research/feature2-web-provisioning.md` (F2).

**Two features, one approval:**
1. **Keychron BLE receiver** — the board becomes a "USB Bluetooth receiver" for a Keychron K-series
   keyboard: keyboard pairs to the S3 over **BLE (S3 = central/host)**; S3 re-emits keystrokes to the
   PC over **native USB-C as a standard USB HID keyboard**.
2. **Web WIFI + BLE provisioning** — a web page (board opens a **SoftAP**, draws a **QR on the LCD**
   the phone scans) to reconfigure the **home WiFi** without reflashing and to manage the **BLE keyboard**
   pairing — so the board is usable away from home.

User-confirmed choices: keyboard = **Keychron K series**; provisioning UX = **QR on the LCD → setup AP → web page**.

---

## Feature 1 — Keychron BLE keyboard → USB HID bridge

### Architecture
- **USB side (PC sees a keyboard):** the native USB-OTG port runs a **TinyUSB composite device** —
  verified on-board (spike v3) with `ARDUINO_USB_MODE=0` + `ARDUINO_USB_CDC_ON_BOOT=1`: the
  `USBCDC` global (Serial console) and `USBHIDKeyboard` global (HID) register into one `USB.begin()`
  device, so **one USB-C port = HID keyboard + console COM** (MODE=1 = JTAG console only; MODE=2 kills
  the console entirely — the core has no built-in non-TinyUSB OTG-HID path).
- **BLE side (S3 = central/host):** **NimBLE-Arduino** GATT client. Flow: scan → connect to the Keychron
  (Generic HID service **0x1812**) → read **Report Map 0x2A4B** (chunked, ≤ MTU−3) → write **Protocol Mode
  0x2A4E = 0x00** (report mode) → subscribe to **Report 0x2A4D** via its **CCCD 0x2902** → decode the
  standard 64-byte keyboard reports (1 modifier + 1 reserved + 6 keycodes; LED output report 0x01).
- **Bridge:** each 0x2A4D notification → decode → re-emit as a TinyUSB HID keyboard report.
- **Pairing:** standard **"Just Works"** (no PIN). NimBLE completes the bond on accept; store the bond
  (LTK/IRK) in **NVS**; auto-reconnect and re-subscribe the 0x2A4D notification after any reconnect
  (peripherals reset subscriptions).

### Key decisions
- **NimBLE, not legacy `esp_bt` `BLEDevice`:** far lighter host stack (RAM/CPU), clean Just-Works bonding.
- **Avoid the official `esp_hid` NimBLE host component as-is** — open bug: it never reads the HID Report
  Map (esp-idf #19011 / IDFGH-18187). Hand-roll the ~200-line GATT client instead.
- **Working reference:** `JimGat/CYM` (ESP32-S3 central that reads the report map and decodes 0x2A4D
  notifications). Adafruit's "USB→BLE keyboard adapter" is the inverse topology but a good pairing-UX /
  state-machine reference.
- Run the **BLE central + GATT on a dedicated FreeRTOS task** (off the render core; render stays on its
  core with PSRAM framebuffers) so scanning/GATT never starve the ~79-push/5s render or the 15–60 s HTTP poll.

### ✅ R1 resolved — verified on-board (`spike/hid-composite/` "Spike v3", `ARDUINO_USB_MODE=0`)
The S3's internal USB PHY is shared between USB-Serial-JTAG and USB-OTG — but the MODE=0 composite
exposes **both a CDC console (COM) and the HID keyboard on the one USB-C port**, so the running app
**keeps its `Serial` console** (new COM number — TinyUSB CDC replaces COM4; re-point the monitor there)
and `PRESS` scene-cycling still works.
- **Flashing:** the app's CDC port cannot re-enter download mode; flashing goes through **ROM download
  mode (hold BOOT/GPIO0 + reset)** over the same cable (esptool on the ROM's USB-Serial-JTAG). Spike v3
  itself was uploaded that way — the path is proven.
- Live proof the HID works: the v3 spike types `hello from llm-tick usb` + Enter every 2 s into whatever
  PC field has focus (it spammed this very chat GUI while connected). Unplug, or BOOT+reset, to silence.
- **Remaining on-board check:** confirm the composite's normal-op COM number and one BOOT+reset reflash
  once the full-feature firmware is built.

### F1 phases (tracer bullets)
1. **USB-HID spike (gates everything — verify R1):** minimal `ARDUINO_USB_MODE=2` + TinyUSB-HID keyboard
   on the real board. Confirm: (a) PC enumerates a keyboard; (b) `pio run -t upload` still works via
   **BOOT+reset download mode**; (c) the running-app JTAG port is gone as predicted.
2. **BLE central spike:** NimBLE scan + connect to the Keychron in the `Fn+B1` pairing window; confirm a
   Just-Works bond completes, 0x2A4D notifications flow, and log the negotiated connection interval.
3. **Bridge:** BLE notification → decode → USB re-emit; type on the PC end-to-end.
4. **Robustness:** auto-reconnect, re-subscribe on reconnect, guard unknown report IDs, WS2812 link status,
   bond in NVS.
5. **Coexistence:** verify BLE + WiFi STA + render run together (no crash-loop — recall the wifi RX ebuf
   history; keep poll cadence well under ~10/min), no render starvation.

### F1 risks
- **R1 USB PHY handoff** (above) — verify in spike; re-home console; document the new flashing procedure.
- **R2 No turnkey BLE HID host** — hand-rolled NimBLE client (report-map chunked reads).
- **R3 Coexistence RAM/CPU** — NimBLE over `esp_bt`; pin render and BLE to different cores; test under load.
- **R4 Keyboard quirks** — Keychron Just-Works; pairing window ~3 min (spot-check the official PDF);
  connection interval ~7.5–15 ms (bridge adds ~10–30 ms end-to-end — fine for typing).

### ⛔ F1 BLOCKED BY HARDWARE (2026-10-02) — read before touching F1
**The Keychron K8 is a Bluetooth *Classic* (BR/EDR) keyboard, and the ESP32-S3 has no Classic radio.**
The two cannot interoperate at any layer, so no BLE-central change can make the K8 work. Measured
evidence (full detail in `HANDOFF.md` item 24):

- Windows' **Classic** pairing store `HKLM:\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices`
  holds `dc2c26ead3e5 => Keychron K8` (`DC:2C:26:EA:D3:E5`, Telink OUI); the **BLE** store
  `...\BTHLEEnum\Parameters\Devices` is **empty**.
- Across several `Fn+B1` pairing windows, two independent scanners (the Windows WinRT LE watcher and the
  board's NimBLE central) each saw ~50 advertisers and **zero** advertising HID service `0x1812`.
- Espressif: ESP32-S3 = "2.4 GHz Wi-Fi and Bluetooth® 5 (**LE**)" only — BR/EDR was dropped.

**R4 above is the wrong risk.** The blocker is the radio protocol, not the pairing window. Choose one:
1. **Use a BLE HID keyboard** (Keychron **K Pro / K Max / K3 Pro**, or any generic BLE HID keyboard) — the
   existing F1 code then works unchanged. ← cheapest, recommended
2. **Verify against a synthetic BLE HID peer** — an ESP32-S3 *can* be a BLE HID **peripheral**; flash a
   second S3 with a HID-over-GATT keyboard emulator and drive board #1 → USB → PC. Proves every line of F1
   except "the peer is a Keychron". Needs a second board. ← best way to de-risk the code without a keyboard
3. **Change the board** to one with BR/EDR (original ESP32 / ESP32-WROVER + Bluedroid HID Host) if the K8
   itself must be the keyboard — abandons the S3 display pipeline. ← most expensive

F2 (web provisioning) is **unaffected** by this blocker and can proceed independently.

---

## Feature 2 — Web WIFI + BLE provisioning

### Mechanism (recommended): **DIY SoftAP + `WebServer` + NVS (`Preferences`)**
- **Not** the ESP-IDF `wifi_provisioning` component — it's an **ESP-IDF-build** component (bundled web UI +
  BLE GATT scheme), not an Arduino/PlatformIO drop-in; porting it costs more than a small form page.
- **Not** the heavy `WiFiManager` — more moving parts (captive portal, scan list) than we need.
- A ~150-line SoftAP + `WebServer` page (two fields + a BLE section) + NVS is the smallest complete change
  and gives full control of the chosen UX.

### UX flow (as you chose)
1. **Boot** → read NVS namespace `llmtick`. If `ssid`/`prov` present → STA-connect (NVS wins; fall back to
   `secrets.h` defaults only when NVS empty) → normal operation (no AP broadcast).
2. **Enter provisioning** when: not provisioned, **or** STA connect to the stored network fails within
   ~20–30 s, **or** an explicit trigger (serial `PROV` line / long BOOT press). Then: bring up **SoftAP
   `llm-tick-prov` (WPA2)**, draw the **QR** (`WIFI:T:WPA;S:<ssid>;P:<pass>;;`) + SSID/IP text on the LCD,
   start `WebServer` at `192.168.4.1`.
3. **Phone** scans the QR (auto-joins the AP), opens the page, submits **SSID + password** (+ optional
   keyboard-pair action). Board writes NVS, sets the `prov` flag, **shuts the SoftAP off**, (re)connects STA.

### BLE half of the page
- The page's second section manages the **Keychron** pairing: **[Pair]** (start scan+connect; you press
  `Fn+B1` on the keyboard), **[Status/RSSI]**, **[Unpair]**. This is **web UI over the same NimBLE central**
  as Feature 1 — no second BLE mechanism. Driven on the BLE task so it never blocks the render core.

### Code changes
- **New:** `src/prov.cpp/.h` (SoftAP + WebServer + QR + NVS load/save); `src/ble_host.cpp/.h` (NimBLE
  central + HID-over-GATT client + pairing state machine — **shared with Feature 1**); vendor **uQRCode**
  (tiny, dependency-free QR encoder; `WIFI:` string fits QR v4–5, ~4 px/module → scannable on 172×320).
- **Edit:** `src/data.cpp::wifiInit()` (NVS-first credentials + enter-provisioning fallback); `src/main.cpp`
  (setup/loop hooks, a provisioning UI scene, re-home `Serial`/`PRESS` per F1); `platformio.ini`
  (`ARDUINO_USB_MODE=2`, `ARDUINO_USB_CDC_ON_BOOT=0`, add NimBLE-Arduino + uQRCode to `lib_deps`).
- `secrets.h` becomes the **factory default only** (NVS overrides at runtime).

### Security / robustness
- Setup AP on **WPA2** (short passphrase shown on the LCD); optional one-time PIN on the page; set the
  `prov` NVS flag and **auto-shut the SoftAP** after success. **`FACTORY`** serial command = `prefs.clear()`
  + reboot.

---

## Cross-cutting risks & mitigations
| Risk | Mitigation |
|---|---|
| **R1** USB PHY handoff removes COM4 auto-flash + running serial | Verify in F1 spike; re-home console to UART0; document BOOT+reset download-mode flashing |
| **R2** No turnkey BLE HID host (`esp_hid` bug) | Hand-rolled NimBLE GATT client; `JimGat/CYM` reference; chunked report-map reads |
| **R3** BLE + WiFi + render coexistence load | NimBLE (not `esp_bt`); pin render & BLE to different cores; load-test (keep poll < ~10/min) |
| **R4** Keychron pairing-window / interval quirks | Prompt scan/connect state machine; ~3-min window; auto-reconnect + re-subscribe; WS2812 link status |
| **RAM** | Budget ~150–250 KB internal SRAM for the NimBLE host; PSRAM (8 MB) holds the framebuffers |

## Proposed build order
1. ~~**F1-USB-HID spike** (verify the PHY handoff + download-mode flashing)~~ — **DONE (pre-approval,
   `spike/hid-composite/` v3)**: MODE=0 composite keeps console + HID on one port; download-mode flashing
   proven (v3 was uploaded via BOOT+reset).
2. **F1-BLE central + bridge** (pair, type end-to-end).
3. **F2 provisioning** (NVS + SoftAP + Web + QR + BLE section).
4. **Integration:** coexistence stress test, on-PC + on-board verification, update `README.md` / `HANDOFF.md`
   (new flashing procedure, provisioning flow, BLE section).

## Approval (obtained via ask_user_question)
1. **Flash workflow: accepted** (BOOT+reset download mode). R1 spike made it better than assumed: the
   MODE=0 composite **keeps the serial console** (new COM number), so only the flashing procedure changes.
2. **Keychron model:** the UI free-text answers turned out to be the board's own `hello from llm-tick
   usb` typing (spike firmware) — model to confirm in person at the BLE stage (pairing flow is identical
   across K-series: hold Fn ~3 s + B1).
3. **Deps: approved** — vendor NimBLE-Arduino + uQRCode into `lib_deps`.
4. **Setup AP:** `llm-tick-prov` + short WPA2 passphrase shown on the LCD, **no one-time PIN**.

## Sources
- F1: `.scratch/research/feature1-ble-keyboard-usb-bridge.md` (Espressif USB-Serial-JTAG/OTG-Console guides,
  S3 TRM PHY-selection, Arduino USB/CDC APIs, BT SIG HID-over-GATT, `JimGat/CYM`, esp-idf #19011, Keychron manuals).
- F2: `.scratch/research/feature2-web-provisioning.md` (Espressif Preferences/NVS, ESP-IDF wifi_provisioning,
  `esp_wifi` `WIFI_AP_STA`, `WiFiManager`, uQRCode, `WIFI:` URI format).
