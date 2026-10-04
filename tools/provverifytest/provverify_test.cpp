// Host harness for the Provisioning VERIFY seam (src/provverify.h).
//
// The verify phase is the first slice that touches the radio, and its decision logic is
// exactly the part that cannot be observed by reading the firmware: a deadline that
// fires early, a status that maps to the wrong reason, or a terminal state that quietly
// un-latches all produce a board that merely misbehaves on hardware. Waiting 20 s to
// discover that is not a test, so the logic is extracted here and driven directly.
//
// Everything below is Arduino-free: wifiStatus is an int, exactly as WiFi.status()
// returns it, and the WL_* values it is compared against are declared here rather than
// included, so this compiles with a host g++.
//
//   pwsh -File tools\provverifytest\build.ps1
#include <cstdio>
#include <string>

#include "provverify.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  [PASS] %s\n", what); }
    else    { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void check_eq(long got, long want, const char* what) {
    bool ok = got == want;
    if (ok) { g_pass++; printf("  [PASS] %s\n", what); }
    else    { g_fail++; printf("  [FAIL] %s (got %ld, want %ld)\n", what, got, want); }
}

static void check_str(const char* got, const char* want, const char* what) {
    bool ok = got && want && std::string(got) == std::string(want);
    if (ok) { g_pass++; printf("  [PASS] %s\n", what); }
    else    { g_fail++; printf("  [FAIL] %s (got '%s', want '%s')\n", what,
                                 got ? got : "(null)", want ? want : "(null)"); }
}

// ── cycle 1: success is recognised the moment the station associates ──────────
//
// Before this, a Board that connected correctly but stayed in Provisioning would have
// looked identical to one that timed out: both end the phase, both leave the operator
// with a Board that is not on their network.
static void test_connected_is_success() {
    printf("\ncycle 1: an associated station ends the phase successfully\n");

    prov_verify_state_t s = PROV_VERIFY_RUNNING;
    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;

    s = provVerifyPoll(s, 1000, 4000, PROV_VERIFY_BUDGET_MS, WL_CONNECTED, &f);
    check_eq(s, PROV_VERIFY_OK, "WL_CONNECTED while RUNNING is OK");
    check_eq(f, PROV_VERIFY_FAIL_NONE, "and no failure reason is recorded");
}

// ── cycle 2: the budget is the 20 s wifiInit() already uses ───────────────────
static void test_budget_is_twenty_seconds() {
    printf("\ncycle 2: the budget is the same patience wifiInit() uses\n");

    check_eq((long)PROV_VERIFY_BUDGET_MS, 20000L,
             "the verify budget is 20000 ms, not a second invented timeout");
}

// ── cycle 3: the deadline fires ONCE and not early ────────────────────────────
//
// A deadline that fires early is the dangerous direction: the Board would give up on a
// slow-but-working network and tell the operator their password is wrong.
static void test_deadline_does_not_fire_early() {
    printf("\ncycle 3: still trying at 19999 ms, failed at 20000\n");

    prov_verify_state_t s = PROV_VERIFY_RUNNING;
    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;

    s = provVerifyPoll(s, 0, PROV_VERIFY_BUDGET_MS - 1, PROV_VERIFY_BUDGET_MS,
                       WL_IDLE_STATUS, &f);
    check_eq(s, PROV_VERIFY_RUNNING, "one millisecond before the budget it is still running");

    s = provVerifyPoll(s, 0, PROV_VERIFY_BUDGET_MS, PROV_VERIFY_BUDGET_MS,
                       WL_IDLE_STATUS, &f);
    check_eq(s, PROV_VERIFY_FAILED, "at the budget it fails");
    check_eq(f, PROV_VERIFY_FAIL_TIMEOUT, "and the reason is the timeout");
}

// ── cycle 4: a rejected association names the PASSWORD, not the network ───────
//
// This is the reason the whole ticket exists. The operator typed a wrong password and
// must be told exactly that; "could not connect" sends them looking at the router.
static void test_wrong_password_is_named_as_such() {
    printf("\ncycle 4: a refused handshake blames the password\n");

    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;
    prov_verify_state_t s = provVerifyPoll(PROV_VERIFY_RUNNING, 0, 3000,
                                          PROV_VERIFY_BUDGET_MS, WL_CONNECT_FAILED, &f);
    check_eq(s, PROV_VERIFY_FAILED, "a refused handshake ends the phase");
    check_eq(f, PROV_VERIFY_FAIL_BAD_PASSWORD, "blamed on the WiFi password");

    check_str(provVerifyReasonText(PROV_VERIFY_FAIL_BAD_PASSWORD), "wrong-password",
              "and the page can say so");
}

// ── cycle 5: a missing network is a DIFFERENT reason ──────────────────────────
//
// Collapsing these two is how an operator ends up changing a correct password because
// their router is on another channel.
static void test_missing_network_is_distinguished() {
    printf("\ncycle 5: an absent SSID is not blamed on the password\n");

    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;
    prov_verify_state_t s = provVerifyPoll(PROV_VERIFY_RUNNING, 0, 2000,
                                          PROV_VERIFY_BUDGET_MS, WL_NO_SSID_AVAIL, &f);
    check_eq(s, PROV_VERIFY_FAILED, "an absent SSID ends the phase");
    check_eq(f, PROV_VERIFY_FAIL_NETWORK, "blamed on the network, not the password");
    check(f != PROV_VERIFY_FAIL_BAD_PASSWORD, "explicitly NOT a password failure");
}

// ── cycle 6: terminal states latch ────────────────────────────────────────────
//
// Without this the phase would keep polling after it had already decided, and a late
// WL_CONNECTED would overwrite a correct failure - turning a Board that could not join
// into one that claims it did, and rebooting it onto a network it never reached.
static void test_terminal_states_latch() {
    printf("\ncycle 6: a decided outcome is never revisited\n");

    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;
    prov_verify_state_t s = provVerifyPoll(PROV_VERIFY_RUNNING, 0, 3000,
                                          PROV_VERIFY_BUDGET_MS, WL_CONNECT_FAILED, &f);
    check_eq(s, PROV_VERIFY_FAILED, "failed");

    prov_verify_fail_t f2 = PROV_VERIFY_FAIL_NONE;
    s = provVerifyPoll(s, 3000, 3001, PROV_VERIFY_BUDGET_MS, WL_CONNECTED, &f2);
    check_eq(s, PROV_VERIFY_FAILED, "a late WL_CONNECTED does not revive it");
    check_eq(f2, PROV_VERIFY_FAIL_NONE, "and does not rewrite the recorded reason");

    s = provVerifyPoll(PROV_VERIFY_OK, 0, 100, PROV_VERIFY_BUDGET_MS, WL_NO_SSID_AVAIL, &f2);
    check_eq(s, PROV_VERIFY_OK, "success is equally latched");
}

// ── cycle 7: IDLE never starts anything by itself ─────────────────────────────
static void test_idle_is_inert() {
    printf("\ncycle 7: IDLE polls do nothing\n");

    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;
    prov_verify_state_t s = provVerifyPoll(PROV_VERIFY_IDLE, 0, 999999,
                                          PROV_VERIFY_BUDGET_MS, WL_CONNECTED, &f);
    check_eq(s, PROV_VERIFY_IDLE, "polling from IDLE stays IDLE even when connected");
    check_eq(f, PROV_VERIFY_FAIL_NONE, "and invents no reason");
}

// ── cycle 8: millis() wraparound ──────────────────────────────────────────────
//
// uint32 milliseconds wrap every ~49.7 days, and a Board that polls continuously will
// be running across that boundary. A naive `now - started > budget` is then NEGATIVE and
// the deadline either never fires or fires immediately — a bug that would appear on a
// Board left installed for seven weeks and could not be reproduced on the bench.
static void test_deadline_survives_millis_wraparound() {
    printf("\ncycle 8: the deadline survives the 49-day millis() wrap\n");

    const uint32_t budget = PROV_VERIFY_BUDGET_MS;
    // Start 500 ms before the wrap: started = 0xFFFFFFFF - 499, so +20000 crosses zero.
    const uint32_t started = 0xFFFFFFFFu - 499u;
    prov_verify_fail_t f = PROV_VERIFY_FAIL_NONE;

    prov_verify_state_t s = provVerifyPoll(PROV_VERIFY_RUNNING, started, started + 499u,
                                          budget, WL_IDLE_STATUS, &f);
    check_eq(s, PROV_VERIFY_RUNNING, "still running just before the wrap");

    // started + 20000 wraps to 19500.
    s = provVerifyPoll(PROV_VERIFY_RUNNING, started, started + budget, budget,
                       WL_IDLE_STATUS, &f);
    check_eq(s, PROV_VERIFY_FAILED, "and the budget still fires across the wrap");
    check_eq(f, PROV_VERIFY_FAIL_TIMEOUT, "with the timeout reason, not a bogus success");
}

// ── cycle 9: every reason has distinct, non-empty text ────────────────────────
//
// Two reasons sharing a message would make cycles 4 and 5 indistinguishable to the
// operator, which is the whole point of distinguishing them.
//
// FAIL_NONE is excluded on purpose: it means "no failure", and its text is deliberately
// "" so a caller that renders the reason unconditionally shows nothing rather than a
// bogus message beside a successful submit.
static void test_reasons_have_distinct_text() {
    printf("\ncycle 9: each reason has its own non-empty text\n");

    const prov_verify_fail_t real[] = {
        PROV_VERIFY_FAIL_BAD_PASSWORD,
        PROV_VERIFY_FAIL_NETWORK,
        PROV_VERIFY_FAIL_TIMEOUT,
        PROV_VERIFY_FAIL_NO_IP,
    };
    const int n = (int)(sizeof(real) / sizeof(real[0]));

    for (int i = 0; i < n; i++) {
        check(provVerifyReasonText(real[i]) != nullptr &&
              provVerifyReasonText(real[i])[0] != '\0', "a reason has non-empty text");
    }

    check_str(provVerifyReasonText(PROV_VERIFY_FAIL_NONE), "",
              "FAIL_NONE has empty text, not a bogus message");

    check(std::string(provVerifyReasonText(PROV_VERIFY_FAIL_BAD_PASSWORD)) !=
          std::string(provVerifyReasonText(PROV_VERIFY_FAIL_NETWORK)),
          "password and network failures do not share text");
}

int main() {
    printf("\nProvisioning verify seam — deadline, outcomes, latching\n");
    test_connected_is_success();
    test_budget_is_twenty_seconds();
    test_deadline_does_not_fire_early();
    test_wrong_password_is_named_as_such();
    test_missing_network_is_distinguished();
    test_terminal_states_latch();
    test_idle_is_inert();
    test_deadline_survives_millis_wraparound();
    test_reasons_have_distinct_text();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}