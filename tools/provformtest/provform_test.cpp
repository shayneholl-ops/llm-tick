// provform_test.cpp — host tests for the provisioning FORM, the slice that turns the
// Setup AP's read-only page into something an operator can fill in.
//
// WHY A SEPARATE HARNESS
// The form has three rules worth testing that have nothing to do with HTTP or the
// radio, and all three have been wrong before:
//
//   - pre-fill shows what is EFFECTIVE, while unset and explicitly-empty stay
//     distinguishable (CONTEXT.md; conflating them is the bug #7 exists to avoid),
//   - the password never reaches the page,
//   - a rejected submit stores nothing at all.
//
// None of that needs a Board. src/provform.cpp is a separate translation unit with no
// Arduino includes for exactly the same reason src/config.cpp is — so these can run in
// seconds instead of a flash cycle.
//
//   pwsh -File tools\provformtest\build.ps1
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"

extern "C" {
#include "config.h"
}

#include "provform.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

static void checkStr(const char* got, const char* want, const char* what) {
    if (got && strcmp(got, want) == 0) { g_pass++; printf("  ok   %s\n", what); }
    else {
        g_fail++;
        printf("  FAIL %s\n         want: %s\n         got:  %s\n",
               what, want, got ? got : "(null)");
    }
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Reset BOTH the RAM values and the fake NVS store. cfgTestWipe() alone only clears the
// store, so the RAM opt-strings survive from the previous cycle and leak into it — which
// is how cycle 6 came to believe a fresh field was not UNSET.
static void resetConfig() {
    cfgReset();
    cfgTestWipe();
}

// Build a submit from plain C strings, so each test reads as the values an operator
// typed rather than as struct plumbing.
static prov_form_values_t v(const char* ssid, const char* pass,
                            const char* host, const char* ip, const char* weather) {
    prov_form_values_t s;
    s.ssid = ssid; s.pass = pass; s.host = host; s.ip = ip; s.weather = weather;
    return s;
}

// ── cycle 1: every field is pre-filled with the value currently in effect ──────
static void test_prefill_reflects_current_config() {
    printf("\n[cycle 1] the form shows what is currently in effect\n");
    resetConfig();

    cfgSetWifiSsid("living-room-ap");
    cfgSetServerHost("nas.local");
    cfgSetServerIp("10.0.0.7");
    cfgSetWeatherLocation("Vancouver");

    std::string page = provBuildForm(provFormCurrentValues());

    check(contains(page, "living-room-ap"), "WiFi SSID is pre-filled");
    check(contains(page, "nas.local"),    "Server hostname is pre-filled");
    // Assert the IP field carries EXACTLY the expected address. A substring check is not
// enough here: a dangling-pointer bug left a correct "10.0.0.7" in the buffer with
// garbage after it, which contains() happily accepted.
    check(contains(page, "name=\"ip\" value=\"10.0.0.7\""),
          "the Server IP input holds exactly the address in effect");
    check(contains(page, "Vancouver"),    "weather location is pre-filled");
    check(contains(page, "<form"),        "the page actually contains a form");
    check(contains(page, "name=\"ssid\""),   "the SSID field is named ssid");
    check(contains(page, "name=\"pass\""),   "the passphrase field is named pass");
    check(contains(page, "name=\"host\""),   "the hostname field is named host");
    check(contains(page, "name=\"ip\""),     "the Server IP field is named ip");
    check(contains(page, "name=\"weather\""), "the weather field is named weather");
}

// ── cycle 2: the password never reaches the page ──────────────────────────────
static void test_password_is_never_echoed() {
    printf("\n[cycle 2] the passphrase never appears in the page source\n");
    resetConfig();

    cfgSetWifiPass("hunter2-correct-horse");

    std::string page = provBuildForm(provFormCurrentValues());

    check(!contains(page, "hunter2"), "the stored passphrase is not in the page at all");
    check(contains(page, "type=\"password\""), "the passphrase field is a password input");
    check(contains(page, "unchanged"),
          "the passphrase field explains that blank means unchanged");
}

// ── cycle 3: a malformed Server IP is rejected ────────────────────────────────
static void test_rejects_malformed_server_ip() {
    printf("\n[cycle 3] a Server IP that is not an IP is rejected\n");

    prov_form_error_t err = PROV_FORM_OK;
    bool ok = provValidate(v("192.168.1.300", "1", "nas.local", "not-an-ip", "Vancouver"), &err);

    check(!ok, "a non-numeric Server IP is refused");
    check(err == PROV_FORM_ERR_SERVER_IP, "and it is reported as a Server IP problem");
}

// ── cycle 4: an over-long field is rejected ───────────────────────────────────
static void test_rejects_over_long_field() {
    printf("\n[cycle 4] an over-long field is refused rather than truncated\n");
    std::string huge(200, 'x');

    prov_form_error_t err = PROV_FORM_OK;
    bool ok = provValidate(v(huge.c_str(), "pass", "nas.local", "", "Vancouver"), &err);

    check(!ok, "a 200-character SSID is refused");
    check(err == PROV_FORM_ERR_SSID_TOO_LONG, "and it is reported as an SSID problem");

    prov_form_error_t err2 = PROV_FORM_OK;
    bool ok2 = provValidate(v("ap", "pass", huge.c_str(), "", "Vancouver"), &err2);
    check(!ok2 && err2 == PROV_FORM_ERR_SERVER_HOST, "an over-long hostname is refused too");
}

// ── cycle 5: per-field fallback survives a submit ─────────────────────────────
static void test_only_submitted_fields_change() {
    printf("\n[cycle 5] submitting one group leaves the others alone\n");
    resetConfig();

    cfgSetServerHost("nas.local");
    cfgSaveServer();

    // What the form ACTUALLY sends when the operator edits only WiFi: the passphrase is
    // blank (it is never pre-filled), and every other field comes back at its current
    // value because the form pre-fills them. Writing those back is idempotent, so the
    // Server is untouched in effect — that, not a group flag, is what per-field fallback
    // rests on.
    check(provApply(v("kitchen-ap", "secret123", "nas.local", "", ""), nullptr),
          "a WiFi-only submit is accepted");

    checkStr(cfgWifiSsid(), "kitchen-ap", "WiFi SSID changed to what was submitted");
    checkStr(cfgServerHost(), "nas.local", "Server hostname was left untouched");
    check(cfgServerHostIsStored(),
          "the hostname is still STORED, not reset to its Factory default");
}

static void test_blank_passphrase_keeps_the_stored_one() {
    printf("\n[cycle 5b] a blank passphrase means 'unchanged', not 'no password'\n");
    resetConfig();

    cfgSetWifiPass("the-original-secret");
    cfgSaveWifi();

    // The operator changes the SSID and leaves the passphrase box alone.
    check(provApply(v("new-ap", "", "nas.local", "", ""), nullptr),
          "a submit with a blank passphrase is accepted");
    checkStr(cfgWifiPass(), "the-original-secret",
             "the stored passphrase survives — an empty one would lock the Board out");
}

// ── cycle 6: a blank Server IP means no fallback, not the Factory default ─────
static void test_blank_server_ip_means_no_fallback() {
    printf("\n[cycle 6] blanking the Server IP means 'no static fallback'\n");
    resetConfig();

    // Unset: the Factory default IP applies.
    check(cfgServerIpState() == CFG_IP_UNSET, "a fresh field is UNSET");
    check(cfgServerIpIsSet(), "so the Factory default applies");

    check(provApply(v("ap", "pass", "nas.local", "", ""), nullptr),
          "a submit with a blank IP is accepted");

    check(cfgServerIpState() == CFG_IP_EMPTY, "blanking the field makes it explicitly EMPTY");
    check(!cfgServerIpIsSet(), "and there is now NO static fallback at all");
}

static void test_blank_weather_means_no_weather() {
    printf("\n[cycle 6b] blanking the weather location means 'no weather'\n");
    resetConfig();

    cfgSetWeatherLocation("Vancouver");
    cfgSaveWeather();
    checkStr(cfgWeatherLocation(), "Vancouver", "a location is in effect to begin with");

    check(provApply(v("ap", "pass", "nas.local", "", ""), nullptr),
          "a submit with a blank weather location is accepted");
    checkStr(cfgWeatherLocation(), "", "and the location is now explicitly empty");
}

// ── cycle 7: a rejected submit stores nothing ────────────────────────────────
static void test_rejected_submit_stores_nothing() {
    printf("\n[cycle 7] a rejected submit changes nothing\n");
    resetConfig();

    cfgSetWifiSsid("original-ap");
    cfgSaveWifi();

    prov_form_error_t err = PROV_FORM_OK;
    bool ok = provValidate(v("new-ap", "pw", "nas.local", "999.1.1.1", "Vancouver"), &err);
    check(!ok, "the submit is refused for the bad Server IP");

    check(!provApply(v("new-ap", "pw", "nas.local", "999.1.1.1", "Vancouver"), nullptr),
          "and applying the same values is also refused");

    checkStr(cfgWifiSsid(), "original-ap",
             "the SSID is UNCHANGED — a refused submit stores nothing");
}

int main() {
    printf("provform host tests\n=====================\n");

    test_prefill_reflects_current_config();
    test_password_is_never_echoed();
    test_rejects_malformed_server_ip();
    test_rejects_over_long_field();
    test_only_submitted_fields_change();
    test_blank_passphrase_keeps_the_stored_one();
    test_blank_server_ip_means_no_fallback();
    test_blank_weather_means_no_weather();
    test_rejected_submit_stores_nothing();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
