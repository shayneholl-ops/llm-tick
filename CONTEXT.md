# llm-tick

A Waveshare **ESP32-S3-LCD-1.47B** (ST7789 172×320) turned into a desk display for
LLM usage: it polls a small HTTP server (`server.py`) on the LAN and shows session /
weekly token budgets, GPU load, and a weather standby scene. It is read-only — it
displays, it never drives the model.

## Language

**Board**:
The ESP32-S3 device running this firmware. There is exactly one, and it is the thing
the user looks at.
_Avoid_: device, unit, display, panel

**Panel**:
The 172×320 ST7789 glass and its controller. Distinct from the Board, which is the
whole computer: the Panel has quirks (a luminance meander at the RAM-row end, a
cool-cast backlight) that the Board's code works around.
_Avoid_: screen, LCD, display

**Usage feed**:
The JSON payload from `server.py` that populates the usage scene. It is the Board's
only real input; when it stops, the Board is showing stale data rather than nothing.
_Avoid_: data, metrics, stats

**Server**:
The machine running `server.py`, identified by a hostname and an optional static IP.
It is *not* the Board, and it is usually not the machine the Ledger describes.
_Avoid_: backend, host, PC

**Ledger**:
The append-only JSONL file of token counts that `llama_usage_poller.py` writes and
`server.py` reads. It is the single source of truth for usage; the usage feed is a
view of it.
_Avoid_: log, usage log, jsonl

**Standby**:
The Board's weather scene, entered after the usage data has been unchanged for long
enough to conclude nothing is running. It is a *state inferred from staleness*, not a
user-chosen mode.
_Avoid_: idle, screensaver, sleep

**Provisioning**:
The out-of-band flow by which the Board's WiFi credentials and Server address are set
at runtime instead of at compile time. It is entered deliberately, and it ends.
_Avoid_: setup, configuration, onboarding

**Setup AP**:
The temporary access point the Board broadcasts *during* Provisioning, so a phone can
reach the setup page without a PC or cable. It exists only while Provisioning runs —
a WiFi outage never causes one to appear.
_Avoid_: hotspot, config AP, fallback AP

**Provisioned**:
The property of a Board that has values stored in NVS. A provisioned Board never uses
the Factory default, even if a stored value is empty.
_Avoid_: configured, set up

**Verify-then-reboot**:
The rule that a submitted provision is only treated as successful once the Board has
actually authenticated to the new network. The Setup AP stays up until then, so a
failed attempt leaves the page reachable.
_Avoid_: save and restart

**Factory default**:
The values in `secrets.h`. They are what a Board with no provisioned values uses, and
they are never the source of truth once Provisioning has run.
_Avoid_: default config, fallback values

**Config seam**:
The one place the firmware asks for a configuration value, so no consumer needs to know
whether it came from the Factory default or from Provisioning. Code prefix `cfg`.
_Avoid_: settings, options, prefs

**Unset** vs **empty**:
An *unset* configuration field has no stored value, so the Factory default applies. An
*empty* field was deliberately stored as empty and does **not** fall back. The two are
distinct states and must never be conflated.
_Avoid_: blank (ambiguous), missing



