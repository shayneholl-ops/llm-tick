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
- **It adds radio load to a fragile path.** Provisioning runs `AP+STA` so the Board
  keeps its STA link. This Board has a documented history of WiFi-driver heap
  corruption under RX load, and the STA link during an outage is exactly the path
  that is already unhealthy.
- **The failure is already representable.** The Board shows "stale data" and retries
  with exponential backoff (`tickLogic()`). A WiFi outage is a thing to *display*,
  not a thing to reconfigure.

## Decision

Provisioning is entered **only** when the Board has never been provisioned, or on an
**explicit trigger** (serial `PROV`, or a deliberate long BOOT press). A WiFi
connection failure never enters Provisioning: the Board keeps showing its stale-data
state and retrying with backoff.

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
