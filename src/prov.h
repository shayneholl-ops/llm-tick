// prov.h — the Provisioning state: a Setup AP the operator can join to configure the
// Board, plus the Panel screen that tells them how.
//
// ENTERED EXPLICITLY ONLY (ADR-0001). A WiFi connection failure must NEVER enter
// Provisioning: an unattended Board must not start broadcasting an AP because a router
// rebooted. A WiFi outage is displayed as stale data and retried with backoff. The only
// ways in are the serial `PROV` command (and, on hardware that has a working button, a
// deliberate long press — not wired here because this unit's BOOT/GPIO0 is inert).
//
// While Provisioning runs, the radio belongs to the phone: the usage and weather poll
// is SUSPENDED. The Board has a documented history of WiFi-driver heap corruption under
// bursty RX load, and the setup page is a new, heavier load profile than polling.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The Setup AP's identity. Fixed, so the QR payload is fixed and its size is known at
// compile time (a varying payload would make the QR geometry unpredictable).
#define PROV_AP_SSID  "llm-tick-prov"
#define PROV_AP_PASS  "setup1234"       // WPA2, >= 8 chars (the WPA2 minimum)
#define PROV_AP_URL   "http://192.168.4.1/"
// Channel 1 of the non-overlapping 1/6/11. Left to itself the S3 chose channel 9, an
// OVERLAPPING channel, and clients saw the beacon but could not associate. Pin it.
#define PROV_AP_CHANNEL 1
// ONE station. The Board's WiFi driver has a documented heap-corruption crash under
// bursty RX load (HANDOFF item 5), and every additional peer is another source of RX
// bursts into the dynamic buffer pool. A setup page serves one operator at a time, so
// allowing four was never needed.
#define PROV_AP_MAX_STA 1

// Bring up the Setup AP and start serving the placeholder page. Idempotent.
// Returns true if the AP came up.
bool provStart(void);

// Tear the Setup AP down and let the normal pipeline resume. No reboot. Idempotent.
void provStop(void);

// True while the Setup AP is up. The poll gate and the renderer both read this.
bool provActive(void);

// Service the web server. Call from loop() while provActive(); a no-op otherwise.
void provTick(void);

// Enter/leave Provisioning, updating the AP, the LED and the poll gate together so
// they can never disagree. These are what the serial command and the UI call.
void provEnter(void);
void provLeave(void);

// Re-enter Provisioning if it was active before a reboot. Call once in setup(), AFTER
// cfgLoad(). Provisioning is a deliberate state, so it survives a reset: otherwise a
// power blip (or any tool that hard-resets the chip) takes the QR off the glass while
// somebody is trying to focus a camera on it.
void provRestoreIfSaved(void);

// One-line status for the serial console and the boot report.
void provPrintStatus(void);

// The AP's IP as text (for the Panel and diagnostics). "" when not active.
const char* provApIp(void);

// ── Panel screen ─────────────────────────────────────────────────────────────
// Draw the Provisioning screen into a framebuffer: a QR on a FLAT background plus the
// AP name, passphrase and URL. Flat because the animated scene's bright pixels fight
// QR contrast, and a QR that will not scan is a locked-out Board.
//
// `buf` is the active frame buffer (SCREEN_W x SCREEN_H, byte-swapped 565 like every
// other scene).
void provRender(uint16_t* buf, int w, int h);

#ifdef __cplusplus
}
#endif
