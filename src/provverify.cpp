// provverify.cpp — see provverify.h for why this exists and what deliberately is NOT here.

#include "provverify.h"

#include <string.h>

// Wrap-safe elapsed time. Plain unsigned subtraction on uint32 already handles the
// rollover correctly — the difference wraps with the operands — so the only thing that
// must NOT be done is comparing that difference as a SIGNED value, or widening to int64
// and hoping. Written this way deliberately so the intent survives a later "cleanup".
uint32_t provVerifyElapsed(uint32_t startedMs, uint32_t nowMs) {
    return (uint32_t)(nowMs - startedMs);
}

prov_verify_state_t provVerifyPoll(prov_verify_state_t cur,
                                   uint32_t startedMs,
                                   uint32_t nowMs,
                                   uint32_t budgetMs,
                                   int wifiStatus,
                                   prov_verify_fail_t* fail) {
    // Terminal states latch. See the header: a late success must not overwrite a correct
    // failure, because the caller's next action on OK is to tear down the AP and reboot.
    if (cur == PROV_VERIFY_OK || cur == PROV_VERIFY_FAILED) return cur;

    // Only an attempt in flight is polled. IDLE is inert: polling it with WL_CONNECTED
    // must not start anything, or a stray status read would commit the Board.
    if (cur != PROV_VERIFY_RUNNING) return cur;

    // Success first, and BEFORE the deadline. A station that associates in the same
    // millisecond the budget expires did connect, and failing it would tell an operator
    // their correct password is wrong.
    if (wifiStatus == WL_CONNECTED) {
        if (fail) *fail = PROV_VERIFY_FAIL_NONE;
        return PROV_VERIFY_OK;
    }

    // Reasons the driver can state definitively, reported as soon as they are known.
    // Waiting for the full budget to name a refused password would make the Board look
    // hung for 20 s to deliver a message it already knows.
    if (wifiStatus == WL_CONNECT_FAILED) {
        if (fail) *fail = PROV_VERIFY_FAIL_BAD_PASSWORD;
        return PROV_VERIFY_FAILED;
    }
    if (wifiStatus == WL_NO_SSID_AVAIL) {
        if (fail) *fail = PROV_VERIFY_FAIL_NETWORK;
        return PROV_VERIFY_FAILED;
    }

    // Still trying. Give up only once the budget is genuinely spent.
    if (provVerifyElapsed(startedMs, nowMs) >= budgetMs) {
        if (fail) *fail = PROV_VERIFY_FAIL_TIMEOUT;
        return PROV_VERIFY_FAILED;
    }

    return PROV_VERIFY_RUNNING;
}

const char* provVerifyReasonText(prov_verify_fail_t fail) {
    switch (fail) {
        case PROV_VERIFY_FAIL_BAD_PASSWORD:
            return "wrong-password";
        case PROV_VERIFY_FAIL_NETWORK:
            return "network-not-found";
        case PROV_VERIFY_FAIL_TIMEOUT:
            return "timeout";
        case PROV_VERIFY_FAIL_NO_IP:
            return "no-address";
        case PROV_VERIFY_FAIL_NONE:
        default:
            return "";
    }
}

