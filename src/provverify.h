// provverify.h — the DECISION logic of the Provisioning verify phase, with no Arduino
// and no WiFi driver in it.
//
// WHY THIS IS A SEAM AND NOT A LOOP IN prov.cpp
// The verify phase waits up to PROV_VERIFY_BUDGET_MS for a station to associate. Every
// interesting failure in that phase is invisible from the firmware: a deadline that
// fires 200 ms early, a WL_CONNECT_FAILED mapped to "network problem" instead of "wrong
// password", a terminal state that quietly un-latches. Each one produces a Board that
// merely misbehaves, and confirming one on hardware means sitting through 20-second
// waits while hoping to notice. So the decisions are pulled out and driven directly by
// tools/provverifytest.
//
// What deliberately stays in prov.cpp: starting the AP unpinned, calling WiFi.begin(),
// polling WiFi.status(), stopping the AP and rebooting. Those touch hardware and have no
// business being faked.
//
// The one non-obvious piece of arithmetic here is the elapsed-time computation, which
// must be wrap-safe: uint32 milliseconds roll over every ~49.7 days, a Board left
// installed will cross that boundary, and a naive `now - started > budget` is then
// negative. That bug would appear only after seven weeks of unattended operation and
// could not be reproduced on a bench, which is exactly the class this harness exists for.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The WiFi status values this seam interprets. Declared here rather than included from
// WiFi.h so the harness compiles with a host compiler; the values are the ESP32 driver's
// and must stay in step with it.
#define PROV_WIFI_CONNECTED        3
#define PROV_WIFI_NO_SSID_AVAIL    1
#define PROV_WIFI_CONNECT_FAILED   4
#define PROV_WIFI_IDLE             0

#define WL_CONNECTED        PROV_WIFI_CONNECTED
#define WL_NO_SSID_AVAIL    PROV_WIFI_NO_SSID_AVAIL
#define WL_CONNECT_FAILED   PROV_WIFI_CONNECT_FAILED
#define WL_IDLE_STATUS      PROV_WIFI_IDLE

// The verify phase's time budget, in ms.
//
// This is the SAME patience wifiInit() already uses (20 iterations of 1 s), not a second
// invented timeout. A Board that eventually associates at 25 s is a problem the normal
// path already has; the verify phase answers a narrower question — "are these
// credentials right?" — and answering it slowly is what makes an operator think the
// button is broken.
#define PROV_VERIFY_BUDGET_MS 20000

typedef enum {
    PROV_VERIFY_IDLE    = 0,  // no attempt in flight
    PROV_VERIFY_RUNNING = 1,  // attempt in flight, within budget
    PROV_VERIFY_OK      = 2,  // associated: terminal
    PROV_VERIFY_FAILED  = 3   // gave up, with a reason: terminal
} prov_verify_state_t;

typedef enum {
    PROV_VERIFY_FAIL_NONE = 0,
    PROV_VERIFY_FAIL_BAD_PASSWORD,  // association refused: the password is wrong
    PROV_VERIFY_FAIL_NETWORK,       // the SSID was not found at all
    PROV_VERIFY_FAIL_TIMEOUT,       // still associating when the budget ran out
    PROV_VERIFY_FAIL_NO_IP          // associated, but never got an address
} prov_verify_fail_t;

// Advance the state machine by one poll. PURE: no globals, no I/O, no Arduino.
//
// `startedMs` and `nowMs` are the same clock, in ms, and may straddle a uint32 wrap.
// `wifiStatus` is WiFi.status() verbatim. On entry `cur` is the current state; the
// returned state is the next one. `fail` receives the reason and is only written when
// the phase ends in failure.
//
// Terminal states latch: once OK or FAILED is returned, every later call returns it
// unchanged and does NOT rewrite `fail`. Without that, a late WL_CONNECTED arriving
// after a correct timeout would flip a Board that could not join into one that claims
// it did, and the caller would tear down the AP and reboot it onto a network it never
// reached.
prov_verify_state_t provVerifyPoll(prov_verify_state_t cur,
                                   uint32_t startedMs,
                                   uint32_t nowMs,
                                   uint32_t budgetMs,
                                   int wifiStatus,
                                   prov_verify_fail_t* fail);

// Short operator-facing reason text for a failure, for the page and the Panel.
// Never NULL; PROV_VERIFY_FAIL_NONE yields "" rather than a bogus message.
const char* provVerifyReasonText(prov_verify_fail_t fail);

// Milliseconds elapsed since `startedMs`, wrap-safe. Exposed because the Panel wants to
// show remaining time and must compute it the same way the deadline does.
uint32_t provVerifyElapsed(uint32_t startedMs, uint32_t nowMs);

#ifdef __cplusplus
}
#endif