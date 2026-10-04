# Provisioning is entered explicitly, never automatically on connection failure

## Status

accepted (2026-10-02)

## Context

Feature 2 gives the Board a runtime way to set its WiFi credentials and Server
address, replacing the compile-time `secrets.h` values. The originating plan
(written while the BLE keyboard feature was still alive, and while the Board was
expected to travel) specified three ways to *enter* Provisioning:

1. the Board has never been provisioned, or
2. the STA connect to the stored network fails within ~20–30 s, or
3. an explicit trigger (serial `PROV`, or a long BOOT press).

Option 2 is the surprising one. It reads as harmless convenience, but in a
stationary deployment it means:

- **A transient outage becomes a persistent hotspot.** `wifiInit()` gives up after
  20 s (`40 × delay(500)`), and the SoftAP has no exit condition except a submitted
  form. A router reboot therefore leaves the Board broadcasting a WPA2 AP in the
  house indefinitely, on a passphrase shown on a screen nobody is looking at.
- **It cannot fix the likely cause.** Auto-entering Provisioning is only useful when
  the *credentials* changed. If the router is rebooting or the Board is out of
  range, an AP on the Board is not a thing you can use to repair either.
- **It adds radio load to a fragile path.** This Board has a documented history of WiFi-driver heap
  corruption under RX load, and the STA link during an outage is exactly the path
  that is already unhealthy.
- **The failure is already representable.** The Board shows "stale data" and retries
  with exponential backoff (`tickLogic()`). A WiFi outage is a thing to *display*,
  not a thing to reconfigure.

## Amendment 1 (2026-10-03): Provisioning runs AP-only, not AP+STA

**Contradicts the Context section above**, which justified the decision partly on the
claim that "Provisioning runs `AP+STA` so the Board keeps its STA link". That is not
what shipped.

Measured on this unit, the Setup AP is brought up with `WiFi.mode(WIFI_AP)` — AP-only —
and the usage/weather poll is suspended for as long as it is up (`tickLogic()`'s
Provisioning gate). Two reasons:

1. **The single radio cannot do both on a chosen channel.** In AP+STA the AP is
   forced onto the *station's* channel, so the channel argument is silently ignored
   (measured: the STA sat on channel 9 and the AP stayed on 9 despite asking for 1).
   Channel 9 is an OVERLAPPING channel, and in this environment clients saw the beacon
   and then could not associate. AP-only gives the AP a free choice, which is what
   makes the pinned channel 1 achievable at all.
2. **Auto-reconnect must be off.** `wifiInit()` enables it; left on with no reachable
   station it retries, and each retry re-initialises the WiFi driver, which tears the
   AP down. That is precisely the "connected, then dropped after a few seconds" symptom
   on the phone. It is disabled *before* the mode switch.

**Consequence:** a Board in Provisioning has no STA link, so the credential check that
ticket #4's verify-then-reboot needs cannot use the live link. #4 must switch to
AP+STA for that one phase, and this ADR is the place to record whether that reintroduces
the channel problem. It does not weaken the decision: the reason AP+STA was attractive
was to keep a link during an outage, and the outage case is now displayed rather than
reconfigured — the second and third bullets of the Context section still carry it.

## Amendment 2 (2026-10-03): a fourth entry path exists

**Contradicts the Decision below**, which admits entry only for a never-provisioned
Board or an explicit trigger.

There are in fact four, and the Decision is amended to name all of them:

| Path | Intent | Source |
|---|---|---|
| Never provisioned | A Factory-fresh Board has no stored credentials, so it cannot connect to anything. It offers the setup path instead of retrying a network nobody gave it. | `provEnterIfNeverProvisioned()`, spec f2:44 |
| Explicit trigger | The deliberate operator action the Decision asks for. | serial `PROV` |
| Long BOOT press | Spec'd but not wired: this unit's BOOT/GPIO0 is inert. | — |
| **Restored flag** | Provisioning is a deliberate state that survives a reset, so a power blip or a hard reset mid-setup does not take the QR off the Panel while an operator is focusing a camera on it. | `provRestoreIfSaved()` |

The restored flag is the addition, and it is the narrowest of the four: it can only
re-enter Provisioning if the Board was *already* in Provisioning when it last saved
state. It cannot turn a network problem into a hotspot, because the condition required
is "this Board was deliberately being provisioned", not "this Board could not connect".

Note the first path deliberately keys on "no WiFi SSID was **ever stored**"
(`cfgWifiIsStored()`), which is a one-shot fact about the Board's history. It is
explicitly **not** the connect-failure path the Decision forbids, and the gate in
`tickLogic()` keeps them apart.

## Decision

Provisioning is entered **only** when the Board has never been provisioned, or on an
**explicit trigger** (serial `PROV`, or a deliberate long BOOT press), or restored from
its own persisted state (Amendment 2). A WiFi connection failure never enters
Provisioning: the Board keeps showing its stale-data state and retrying with backoff.

## Consequences

- The SoftAP is never broadcast as a side effect of a network problem, so an
  unattended Board cannot become an unmanaged access point.
- A user who cannot reach the Board over the network must still reach it over the
  serial console or physically, to trigger Provisioning. This is a deliberate
  trade: the Board is a desk device with a USB cable attached, so the trigger is
  always available.
- "Changed my WiFi password" is the case Provisioning actually serves, and the user
  is present for it by definition.
- Because entry is explicit, the provisioning screen can be a deliberate, blocking
  state rather than something that must coexist with normal rendering.
