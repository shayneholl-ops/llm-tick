# Feature 2 — Web WiFi provisioning (runtime configuration for the Board)

**Status:** designed, not built. Supersedes the F2 half of `PLAN-ble-receiver-provisioning.md`.

## Why

Today every value the Board needs is a compile-time macro in `secrets.h`:

| Setting | Macro | Runtime-changeable? |
|---|---|---|
| WiFi SSID / password | `WIFI_SSID` / `WIFI_PASS` | no |
| Server address | `SERVER_HOST` / `SERVER_IP_OCTETS` | no |
| Weather location | `WEATHER_LOCATION` | no |
| Weather API key | `WEATHER_API_KEY` | **stays compile-time (out of scope)** |

Changing any of them means editing a gitignored header and reflashing over a BOOT+RESET
window. Two motives drive F2, and both are real:

1. **Ergonomics** — reconfiguring currently costs a reflash; the Board lives on a desk
   with a cable, but the *edit* is still a build.
2. **Mobility** — the Board sometimes moves to another network it also controls, where
   there is no PC to flash from.

The Board's own serial console could carry this in ~15 lines (`SETSSID`), and that was
considered. The web page is justified by motive 2: configuring from a phone, with no PC
and no cable. **Rejected:** ESP-IDF `wifi_provisioning` (an ESP-IDF component, not an
Arduino drop-in) and `WiFiManager` (large, opinionated, and its captive portal is
unreliable on some phones). **Chosen:** DIY SoftAP + `WebServer` + NVS via `Preferences`,
which is the smallest complete change and gives exact control of the screen UX.

## The flow

1. **Boot.** Read NVS namespace `llmtick`.
   - **Provisioned** → use the stored values. Any *empty* stored value means "no value",
     never "fall back to `secrets.h`" — see ADR-0001's sibling below.
   - **Never provisioned** → use the Factory default from `secrets.h`, and enter
     Provisioning (a fresh Board should offer the setup path immediately).
2. **Connect.** STA-connect with the resolved credentials.
   - Success → normal operation. The Setup AP never appears.
   - Failure → **display the failure and retry with backoff.** Never enter Provisioning
     on a connection failure (ADR-0001).
3. **Provisioning**, entered only explicitly (serial `PROV`, long BOOT press, or a
   never-provisioned Board):
   - Bring up the **Setup AP** (`llm-tick-prov`, WPA2, passphrase shown on the Panel).
   - **Suspend the usage/weather poll** for as long as the Setup AP is up — the radio
     serves the phone only. This is a deliberate concession to the documented
     WiFi-RX-load crash history.
   - Render a **QR on a flat background** (`WIFI:T:WPA;S:…;P:…;;`), plus the AP SSID,
     passphrase and `http://192.168.4.1` as text. Flat, because a QR that does not scan
     is a locked-out Board.
   - Set the WS2812 ring to a **distinct Provisioning colour**, so setup mode is visible
     without looking at the glass.
   - Serve the form at `192.168.4.1`: **WiFi SSID, WiFi password, Server hostname,
     Server IP (optional), Weather location**. Current values are pre-filled.
4. **Submit → verify → reboot.**
   - Validate: fields non-empty where required; the weather location is validated
     against WeatherAPI and the resolved place name is echoed back to the page.
   - Write NVS, then **attempt a real STA connect using the new credentials while the
     Setup AP stays up** (`AP+STA` exists precisely for this).
   - Connected → mark provisioned, tear down the Setup AP, reboot.
   - Not connected → **stay in Provisioning**, show the error on both the Panel and the
     page, keep the entered values in the form. A typo must not lock the user out.

## Decisions this design settles

- **Entering Provisioning is explicit** — ADR-0001. A WiFi outage is displayed, not
  reconfigured; otherwise a router reboot leaves an unattended Board broadcasting an AP.
- **Server is two fields.** Hostname *and* an optional static IP, because today's
  `resolveServer()` silently falls back to the compile-time IP when mDNS fails — which
  points at the *wrong box* and lies about it. A blank IP field now means "no fallback";
  an unresolved hostname becomes a visible error.
- **Weather location is free text, validated on submit.** Unvalidated, a typo yields a
  Panel that shows no weather and explains nothing.
- **Per-field fallback.** An unset field keeps the Factory default; provisioning WiFi
  alone must not force the operator to retype a Server address.
- **`secrets.h` remains the Factory default**, never the runtime source once provisioned.
- **Wipe path:** a `FACTORY` serial command clears NVS and reboots.

## Build order

**Spike the NVS read-back first.** The web page is the visible work but not the risky
work: the page is a form, a POST and a `putString`. The risk is the read-back shim —
`wifiInit()`, `resolveServer()` and the weather fetch all read compile-time macros today,
and a subtly wrong read (blank NVS, empty string, wrong namespace, `Preferences` left
open) fails as **the Board never connects and you cannot reach the page to fix it**.

So, in order:

1. **NVS read-back + serial-injected config**, proven on the real Board with the serial
   console. No HTML, no SoftAP. Settles: namespace/keys, empty-value semantics,
   per-field fallback, `FACTORY`.
2. **SoftAP + QR screen + LED state**, still with serial-injected values. Settles the
   Panel layout and that the AP comes up and goes away cleanly.
3. **The form** — the thin layer over an already-proven path.
4. **Stress check** — open the page, submit, repeat N times, watching for the WiFi-RX
   crash signature. "It worked once on the desk" is the exact evidence that missed this
   bug the first time.

## Open question for step 1

`Preferences` is flash-backed and the Board currently does **no** NVS writes at runtime.
Whether a write during Provisioning should be followed by a reboot (which this design
does) or a `prefs.end()` + reconnect has not been measured on this unit. Step 1 answers
it, and the answer goes in the HANDOFF.
