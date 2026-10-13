// prov.cpp — the Provisioning state. See prov.h for the rules it implements.
//
// Three halves:
//   1. the Setup AP + the provisioning web page (radio side)
//   2. the verify phase: AP+STA for up to 20 s, then reboot or stay (ticket #8),
//      and the resolve phase: the weather location checked against the weather
//      service after the station connects, before the reboot (ticket #9)
//   3. the Panel screen: a QR on a flat background (drawing side)
//
// The QR matrix is built ONCE, when Provisioning is entered, and cached — rendering it
// per frame would re-run the encoder 79 times a second for a static image.
#include "prov.h"
#include "tick.h"
#include "config.h"
#include "secrets.h"     // WEATHER_API_KEY only — compile-time, like weather_api.cpp

#include "provform.h"
#include "provverify.h"
#include "provresolve.h"
#include "urlutil.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include "qrcode.h"
}

// ── state ────────────────────────────────────────────────────────────────────
static bool g_active = false;
static WebServer* g_server = nullptr;

// Cached QR matrix. Version 1 = 21x21, but sized for up to version 4 (33x33) so a
// longer payload (a rotated passphrase, a longer SSID) still has room without a
// recompile. qrcode_getBufferSize() would give the exact size per version.
static const int kQrMaxModules = 33;
static uint8_t g_qrBuf[kQrMaxModules * kQrMaxModules];
static QRCode  g_qr;
static int     g_qrSize = 0;     // 0 = nothing encoded
static char    g_qrPayload[128];

// Panel geometry. 4 px/module: at version 1 that is (21 + 8) * 4 = 116 px, which leaves
// room for the credentials at a legible size. The quiet zone (4 modules each side) is
// part of the budget — a scanner needs it to lock on.
static const int kQuietModules = 4;

// The flat background and ink. Deliberately NOT the near-black scene canvas: a QR needs
// maximum contrast, and this screen is a utility, not part of the brand surface.
static const uint16_t kBgLogical = 0xFFFF;   // white
static const uint16_t kInkLogical = 0x0000;  // black

// Provisioning LED: a distinct colour so Provisioning is visible without reading the
// glass. Usage is blue, standby is soft green, so this is a warm amber — unmistakable
// next to both, and not confusable with either.
static void provLedOn()  { showLed(kProvLedSentinel); }   // see showLed()

// ── QR ───────────────────────────────────────────────────────────────────────
// Build the "join this network" URI and encode it. Smallest version that fits, so the
// modules stay as large as possible on the glass.
//
// T:WPA2 — NOT T:WPA. In the WIFI: URI format `T:WPA` means WPA1, and the ESP32's
// softAP() with a passphrase advertises WPA2-PSK/CCMP. A phone that reads "WPA" and
// attempts a WPA1 association against a WPA2-only AP fails the handshake, and both
// iOS and Android commonly surface that as "incorrect password" even though the
// passphrase is correct. That is exactly the symptom this fixes. (Verified on the
// glass 2026-10-02: the phone read the QR fine but would not join.)
//
// WHY THE VERSION IS CHOSEN UP FRONT, NOT TRIED (2026-10-03)
// This used to loop versions 1..4 and take the first that returned 0. That is a trap.
// qrcode_initBytes() assigns qrcode->modules and builds a BitBucket over the caller's
// buffer BEFORE it discovers the payload does not fit, and it signals that with an
// early `return -1`. So a rejected attempt leaves `g_qr` and `g_qrBuf` holding a
// half-built state, and simply trying the next version on top of it is undefined
// behaviour. It was, in practice, an immediate reboot loop on this Board:
//   Guru Meditation: Core 1 panic'ed (LoadProhibited), EXCVADDR 0x00000000
//   drawCodewords <- qrcode_initBytes <- qrcode_initText <- encodeQr
// because versions 1 and 2 CANNOT hold this payload (their ECC_LOW byte-mode
// capacities are 17 and 32 bytes) and so both fail before version 3, which does fit.
// LoadProhibited at address 0 is a corrupted pointer, NOT a stack overflow — it
// survived raising CONFIG_MAIN_TASK_STACK_SIZE to 16384 with a byte-identical fault
// (same PC, same A1), which is how it was distinguished from one.
//
// The fix is to ask the capacity question directly and call the encoder ONCE. Byte-mode
// capacity at ECC_LOW, from the library's own tables:
//   v1 17   v2 32   v3 53   v4 78   bytes
// The payload is fixed at compile time (PROV_AP_SSID/PASS are macros), so the version
// is a compile-time fact too — computed here, not discovered at runtime.
static const uint8_t kEccLowByteCapacity[5] = { 0, 17, 32, 53, 78 };   // index = version

static bool encodeQr() {
    snprintf(g_qrPayload, sizeof(g_qrPayload), "WIFI:T:WPA2;S:%s;P:%s;;",
             PROV_AP_SSID, PROV_AP_PASS);

    const size_t need = strlen(g_qrPayload);
    uint8_t ver = 0;
    for (uint8_t v = 1; v <= 4; v++) {
        if (need <= kEccLowByteCapacity[v]) { ver = v; break; }
    }
    if (ver == 0) {
        g_qrSize = 0;
        Serial.printf("[prov] QR payload is %u B — too big for any supported version\n",
                      (unsigned)need);
        return false;
    }
    if (qrcode_getBufferSize(ver) > sizeof(g_qrBuf)) {
        g_qrSize = 0;
        Serial.println("[prov] QR buffer too small for the chosen version");
        return false;
    }

    // Exactly one encoder call. A 0 here is the only success signal the library gives.
    if (qrcode_initText(&g_qr, g_qrBuf, ver, ECC_LOW, g_qrPayload) != 0) {
        g_qrSize = 0;
        Serial.printf("[prov] QR ENCODE FAILED at version %u (%u B payload)\n",
                      ver, (unsigned)need);
        return false;
    }

    g_qrSize = g_qr.size;
    Serial.printf("[prov] QR ok: version %u, %dx%d modules, %u B payload\n",
                  ver, g_qr.size, g_qr.size, (unsigned)need);
    return true;
}

// ── the form ─────────────────────────────────────────────────────────────────
// All the form logic lives in provform.cpp, which has no Arduino includes and is
// host-tested by tools/provformtest. This section is only the wiring: turn a request
// into values, call the seam, render the result. Keeping it thin is what lets the rules
// that actually matter be tested in seconds instead of a flash cycle.

// ── verify phase (ticket #8) ─────────────────────────────────────────────────
// After a successful submit the Board tests the stored credentials before it
// leaves Provisioning (ADR-0001 Amendment 4: write first, verify second). The
// DECISION lives in provverify.cpp (host-tested); here is the wiring that touches
// the radio: AP+STA for the phase's duration, AP-only again afterwards, reboot on
// success.
static prov_verify_state_t g_verifyState = PROV_VERIFY_IDLE;
static uint32_t g_verifyStart = 0;
static prov_verify_fail_t g_verifyFail = PROV_VERIFY_FAIL_NONE;
static char g_verifySsid[33] = "";   // the SSID under test, for logs; the passphrase is never logged

// ── resolve phase (ticket #9) ────────────────────────────────────────────────
// After a station connect the Board asks the weather service what place name the
// operator typed, so "Vancouver" and "Vancouvr" lead to different outcomes. The
// DECISION lives in provresolve.cpp (host-tested); here is the wiring: ONE search
// call — the same 8 s connect / 10 s receive the normal weather fetch uses, no
// retry loop (this radio's RX heap has a history, see HANDOFF item 5) — and the
// result on the page before the Board leaves Provisioning.
//
// A resolve failure is a SURVIVABLE failure, shaped like a verify failure: the AP
// comes back on channel 1, the written values stay, the flag is NOT set, and the
// operator corrects and Saves again. The two ways it can fail must stay distinct
// on every surface: "no such place" vs "the service could not be reached".
// The failure message travels as a pair: the full sentence for the page
// (Amendment 3) and the short reason for the Panel footer and the serial log.
// One struct so the pair is always composed together and read together.
struct ProvFailMessage {
    char notice[160];
    char reason[48];
};
static ProvFailMessage g_fail = {};
static char g_resolveEcho[160] = ""; // "Weather resolved: …" — shown before the reboot
static uint32_t g_resolveGraceUntil = 0;
static const uint32_t kResolveGraceMs = 8000;  // ≥ one page auto-refresh (5 s)

static void doReboot() { ESP.restart(); }

// The Provisioned flag — the single safety invariant of write-first (ADR-0001
// Amendment 4). Set ONLY when the Board's configuration is confirmed complete: a
// successful verify, or a deliberate submit that left no network to verify. A
// FAILED verify never sets it. Nothing else writes it, and FACTORY (which clears
// this same NVS namespace) is the only thing that clears it.
static const char* kProvDoneKey = "provdone";
static void provSaveProvisioned(bool on) {
    Preferences p; p.begin("llmtick", false); p.putBool(kProvDoneKey, on); p.end();
}
static bool provIsProvisioned() {
    Preferences p; p.begin("llmtick", true);
    bool on = p.getBool(kProvDoneKey, false);
    p.end();
    return on;
}

// THE commit point of write-first (ADR-0001 Amendment 4): set the Provisioned
// flag, take the AP down, reboot onto the verified network. Every successful
// path funnels through here, so "the single writer of the flag" lives in one
// place. (The no-network submit sets the flag on its own path — there is no
// verified network there to reboot onto, so it has its own log line.)
static void provCommitAndReboot() {
    provSaveProvisioned(true);
    provLeave();   // AP down, station properties restored
    Serial.println("[prov] rebooting onto the verified network");
    Serial.flush();
    doReboot();
}

// The one sentence the page shows while the verify phase runs (Amendment 3: the
// outcome lands on BOTH surfaces — page and Panel). One constant, two call sites.
static const char* kVerifyingNotice =
    "Verifying — the Board is testing the connection. Stay connected; this page stays up.";

// Begin the verify phase. The AP stays up for the phase's whole duration: a failed
// attempt must still be able to show its reason on the page (Amendment 3). In
// AP+STA the AP is forced onto the station's channel, so the pinned channel 1 is
// suspended for the phase — which is fine, because the operator's phone is
// ALREADY connected, and the pin's job is to let a phone JOIN, not to hold a
// session (Amendment 3).
static void provVerifyBegin(const char* ssid) {
    WiFi.setAutoReconnect(false);   // the trap: retries re-init the driver and tear the AP down
    WiFi.disconnect(false, false);  // clear any idle STA state before the mode switch
    WiFi.mode(WIFI_AP_STA);
    snprintf(g_verifySsid, sizeof(g_verifySsid), "%s", ssid);
    const char* pass = cfgWifiPass();
    if (pass && *pass) WiFi.begin(ssid, pass);
    else               WiFi.begin(ssid);
    g_verifyFail = PROV_VERIFY_FAIL_NONE;
    g_verifyStart = millis();
    g_verifyState = PROV_VERIFY_RUNNING;
    Serial.printf("[prov] verify: testing ssid='%s' (budget %u s; the AP stays up)\n",
                  ssid, (unsigned)(PROV_VERIFY_BUDGET_MS / 1000));
}

// ONE attempt to ask the weather service what a place name means (ticket #9).
// The same budgets the normal weather fetch already runs — 8 s to connect, 10 s
// to receive — and, deliberately, NO retry loop: this radio's RX heap has a
// history under bursty traffic (HANDOFF item 5), and one unanswered call is
// enough to say "the service could not be reached".
static prov_resolve_outcome_t provResolveAttempt(const char* location,
                                                char* name, size_t nameCap,
                                                char* where, size_t whereCap) {
    String q;
    urlEncode(location, q);
    String url = "http://api.weatherapi.com/v1/search.json?key=" + String(WEATHER_API_KEY)
               + "&q=" + q + "&lang=en";
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.begin(url);
    int code = http.GET();
    String payload = (code == 200) ? http.getString() : String("");
    http.end();
    if (code != 200) return PROV_RESOLVE_ERROR;   // the service could not be reached
    return provResolveParse(payload.c_str(), payload.length(), name, nameCap, where, whereCap);
}

// What a FAILED verify or a FAILED resolve both do to the radio: back to AP-ONLY,
// the AP re-pinned to channel 1. In AP+STA the AP had been forced onto the
// station's channel, and the unpinned state must never outlive the phase
// (Amendment 3). The phone, already associated, follows the beacon — SSID, BSSID
// and passphrase are unchanged.
static void provRestoreApOnly(const char* kind, const char* reason) {
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(PROV_AP_SSID, PROV_AP_PASS, PROV_AP_CHANNEL, 0, PROV_AP_MAX_STA);
    Serial.printf("[prov] %s FAILED (%s) — AP-only ch%d restored; values kept, correct and Save again\n",
                  kind, reason, PROV_AP_CHANNEL);
}

static void sendForm(const prov_form_values_t& v, const char* notice, bool isError) {
    // While the verify phase is active the page auto-refreshes, so a phone holding it
    // sees the outcome on its own — the reason lands on the page AND the Panel
    // without a manual reload (Amendment 3).
    std::string page = provBuildForm(v, notice, isError,
                                     g_verifyState != PROV_VERIFY_IDLE ? 5 : 0);
    g_server->send(200, "text/html", page.c_str());
}

static void handleRoot() {
    // The METHOD and the FULL URI are both printed, and the URI must be the real one.
    // An earlier version logged a literal "GET /", which made a form submitted as
    // GET /?ssid=... indistinguishable from a plain page load — so "no POST in the log"
    // proved nothing at all, because the submit may never have been a POST.
    Serial.printf("[prov] %s %s  from %s\n",
                  g_server->method() == HTTP_POST ? "POST" : "GET",
                  g_server->uri().c_str(),
                  g_server->client().remoteIP().toString().c_str());
    // While the verify phase runs the page carries the phase's status (Amendment 3:
    // the outcome lands on BOTH surfaces — page and Panel).
    if (g_verifyState == PROV_VERIFY_RUNNING) {
        sendForm(provFormCurrentValues(),
                 kVerifyingNotice,
                 false);
    } else if (g_verifyState == PROV_VERIFY_OK && g_resolveEcho[0]) {
        // The resolved location, echoed while the Board waits out the grace
        // window before it reboots (ticket #9: the operator sees what was
        // understood BEFORE it takes effect).
        sendForm(provFormCurrentValues(), g_resolveEcho, false);
    } else if (g_verifyState == PROV_VERIFY_FAILED) {
        // The sentence was composed when the failure happened (verify or
        // resolve), so both surfaces — page and Panel — show the same reason.
        sendForm(provFormCurrentValues(), g_fail.notice, true);
    } else {
        sendForm(provFormCurrentValues(), nullptr, false);
    }
}

// Copy a request argument into caller-owned storage.
//
// WHY THIS EXISTS, and why it is not optional: `WebServer::arg()` returns `String` BY
// VALUE. Writing
//     v.ssid = g_server->arg("ssid").c_str();
// leaves v.ssid pointing into a temporary that is destroyed at the end of that
// statement, so the pointer is already dangling before the next line runs. It reads
// correctly far more often than not — an ABSENT argument returns a String backed by a
// shared static empty buffer — so the failure only shows up for a non-empty field whose
// heap gets reused by the very next allocation, which is exactly what provApply() does.
//
// This is the same defect class as the g_ipText bug this slice already fixed, in the one
// place the host tests cannot reach: the seam is testable, this wiring is not.
static void copyArg(const char* name, char* dst, size_t cap) {
    String raw = g_server->arg(name);
    raw.trim();                       // a trailing space in an HTML field is not a value
    if (raw.length() >= cap) raw = raw.substring(0, cap - 1);
    strncpy(dst, raw.c_str(), cap - 1);
    dst[cap - 1] = '\0';
}

// POST / — validate, store, and re-render the form.
//
// Deliberately does NOT connect, tear the AP down, or reboot: verify-then-reboot is the
// next slice, and this one is the zero-radio-risk slice. The operator can confirm their
// values persisted by reloading, which is what makes it demoable on its own.
//
// A refused submit re-renders with the operator's OWN values still in the boxes, not the
// stored ones, so a typo is correctable in place instead of being silently discarded —
// the anti-lockout property the whole feature exists for (docs/spec/f2-web-provisioning.md:
// "keep the entered values in the form"). The passphrase is among the values passed back,
// and provBuildForm() never renders it, so passing it here leaks nothing.
static void handlePost() {
    Serial.printf("[prov] POST /       from %s\n", g_server->client().remoteIP().toString().c_str());

    // STATIC, not local: provApply() and provBuildForm() both read these after this
    // function's frame is gone from consideration, and nothing here is re-entrant (the
    // web server handles one request at a time on this Board).
    static char ssidBuf[64], passBuf[80], hostBuf[80], ipBuf[32], weatherBuf[80];
    static char oSsidBuf[64], oHostBuf[80], oIpBuf[32], oWeatherBuf[80];

    copyArg("ssid",    ssidBuf,    sizeof(ssidBuf));
    copyArg("pass",    passBuf,    sizeof(passBuf));
    copyArg("host",    hostBuf,    sizeof(hostBuf));
    copyArg("ip",      ipBuf,      sizeof(ipBuf));
    copyArg("weather", weatherBuf, sizeof(weatherBuf));

    // What the form was DISPLAYING when the operator submitted it, per provApply()'s
    // contract. A missing or tampered hidden field falls back to the submitted value, so
    // the worst case is "store what was typed" rather than "store nothing".
    copyArg("o_ssid",    oSsidBuf,    sizeof(oSsidBuf));
    copyArg("o_host",    oHostBuf,    sizeof(oHostBuf));
    copyArg("o_ip",      oIpBuf,      sizeof(oIpBuf));
    copyArg("o_weather", oWeatherBuf, sizeof(oWeatherBuf));

    prov_form_values_t v;
    v.ssid = ssidBuf; v.pass = passBuf; v.host = hostBuf;
    v.ip = ipBuf;     v.weather = weatherBuf;

    prov_form_values_t orig;
    orig.ssid = oSsidBuf; orig.pass = ""; orig.host = oHostBuf;
    orig.ip = oIpBuf;     orig.weather = oWeatherBuf;

    prov_form_error_t err = PROV_FORM_OK;
    if (!provApply(v, orig, &err)) {
        Serial.printf("[prov] submit REFUSED (error %d) — nothing stored\n", (int)err);
        sendForm(v, provFormErrorText(err), true);   // typed values kept, not the stored ones
        return;
    }

    // The passphrase is never logged: this line reaches a serial log that ends up in
    // screenshots and CI output.
    Serial.printf("[prov] submit saved: ssid='%s' host='%s' ip='%s' weather='%s'\n",
                  v.ssid, v.host, v.ip, v.weather);

    // Now verify what was just written (Amendment 4: write first, verify second) —
    // testing the STORED credentials, i.e. exactly what the next boot will use.
    const char* ssid = cfgWifiSsid();
    if (!*ssid) {
        // A submit that leaves no network is a complete configuration: there is
        // nothing to connect to, so there is nothing to verify. A deliberate "no
        // network" must not resurrect the Setup AP on the next boot, so the flag is
        // set here rather than only after a successful verify (Amendment 5).
        Serial.println("[prov] no network configured — nothing to verify; configuration complete");
        provSaveProvisioned(true);
        provLeave();
        doReboot();
        return;
    }
    provVerifyBegin(ssid);

    // The page reports the phase while it runs; the form stays pre-filled from the
    // saved config, so a failure is corrected and resubmitted in place.
    sendForm(provFormCurrentValues(),
             kVerifyingNotice,
             false);
}

// NOTE: the old GET /cfg JSON endpoint is GONE, deliberately.
//
// It existed only to feed the placeholder page's JavaScript, which the form replaced. Left
// registered it would have become a liability rather than dead weight: it built JSON with
// snprintf and NO escaping, and while its inputs were Factory defaults that was harmless —
// but the form makes them operator-supplied, reachable by anyone on the Setup AP. A
// hostname containing a quote would produce malformed JSON, and three 63-character fields
// could overflow the fixed buffer mid-string.
//
// If a machine-readable view is wanted later it should be added deliberately, with
// escaping, rather than inherited from a page that no longer exists.

static void handleNotFound() {
    // WHY THIS LOG IS HERE, and why it is not obvious.
    // A form POST that does not match a registered handler lands here, gets a 302 to the
    // Provisioning page, and the browser FOLLOWS that redirect with a GET — so the symptom
    // is "I pressed Save and it just reloaded the page", with the serial log showing a
    // GET and no POST at all. Exactly what was observed. Without this line the mismatch
    // is invisible, because the POST never reaches any handler to announce itself.
    Serial.printf("[prov] NO HANDLER for %s %s -> 302 redirect\n",
                  g_server->method() == HTTP_POST ? "POST" : "GET",
                  g_server->uri().c_str());

    // Captive-portal nudge: phones probe for a known URL (Apple: /hotspot-detect.html,
    // Android: /generate_204) to decide whether a network needs a sign-in page.
    // Answering anything unknown with a 302 to the Provisioning page is what makes the
    // "Sign in to network" sheet appear — without it, joining the AP looks like it
    // worked but nothing opens, which reads as "no IP redirect".
    g_server->sendHeader("Location", PROV_AP_URL, true);
    g_server->send(302, "text/plain", "");
}

// The captive-portal probes themselves. Answering these with a REDIRECT is what
// triggers the OS sheet; a 200 would tell the phone "this network is fine" and it
// would never offer the page.
static void handleProbe() {
    Serial.printf("[prov] probe %s from %s -> redirect\n",
                  g_server->uri().c_str(),
                  g_server->client().remoteIP().toString().c_str());
    handleNotFound();
}

bool provStart() {
    if (g_active) return true;

    // Pin the AP to channel 1 and a 20 MHz width.
    //
    // WHY: left to itself the S3 picked channel 9, and clients saw the beacon in a scan
    // but failed to associate ("The specific network is not available", RSSI 255 from
    // Windows). Channel 9 is an OVERLAPPING channel — nothing uses it as a primary —
    // and this environment has ~13 other 2.4 GHz APs, so the beacon was visible but
    // unusable. Channels 1, 6 and 11 are the only non-overlapping ones; 1 is the
    // quietest here.
    //
    // IMPORTANT, and the reason the channel is now enforced in AP-only mode: with
    // WIFI_AP_STA the single radio forces the AP onto the STATION's channel, so the
    // channel argument is silently ignored (measured: the STA was on channel 9, and the
    // AP stayed on 9 despite asking for 1). AP-only gives the AP a free choice.
    //
    // TURN OFF AUTO-RECONNECT FIRST. wifiInit() enables it, and it is a STATION
    // feature: left on with no reachable STA it keeps retrying, and each retry
    // re-initialises the WiFi driver. On a single radio that tears the AP down,
    // which shows up on the phone as "connected, then dropped after a few seconds".
    // Disabling it must happen BEFORE the mode switch.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);   // drop any lingering STA attempt, keep the radio
    WiFi.mode(WIFI_AP);
    // THROTTLE THE RX PATH.
    //
    // This Board has a documented, reproducible heap-corruption crash in the WiFi
    // driver's DYNAMIC RX buffer pool under bursty receive load (HANDOFF.md item 5).
    // The decoded crash is always:
    //   wDev_ProcessRxSucData -> esf_buf_alloc_dynamic -> wifi_malloc -> tlsf_malloc
    // A client that associates and then talks (DHCP, DNS, mDNS, ARP) fires exactly the
    // burst that corrupts the pool.
    //
    // It is FIXED STRUCTURALLY in sdkconfig.defaults, for the esp32s3-idf env that
    // compiles the driver from IDF source: DYNAMIC_RX_BUFFER_NUM=0 means the dynamic
    // pool does not exist, rather than being provoked less often. (Measured 2026-10-02:
    // the Board still panicked ~1 s after `stations -> 1` with the web server compiled
    // out, which exonerated the HTTP code and pointed at the radio.)
    //
    // The setting below is the SECOND line of defence, kept because this function also
    // runs in env:esp32s3 — the legacy plain-Arduino build, which links the PREBUILT
    // driver and therefore IGNORES sdkconfig.defaults entirely. Modem sleep makes the
    // radio wake on a schedule instead of accepting every inbound frame the instant it
    // arrives, which is the burst pattern that corrupts the pool. Latency is irrelevant
    // for a Provisioning page. provStop() turns it back off, because it is a station
    // property too and the normal poll should not pay for it.
    WiFi.setSleep(true);
    // Raise TX power for the AP. wifiInit() sets 13 dBm with the note "S3s fail auth at
    // full TX power" — that was about the STATION joining a strong home AP, but the same
    // low power now governs the SETUP AP's beacons and its WPA2 handshake, and a client
    // that cannot complete the 4-way handshake never associates. 17 dBm gives the AP a
    // usable link without going to the 20 dBm maximum.
    WiFi.setTxPower(WIFI_POWER_17dBm);
    if (!WiFi.softAP(PROV_AP_SSID, PROV_AP_PASS, PROV_AP_CHANNEL, 0, PROV_AP_MAX_STA)) {
        Serial.println("[prov] softAP FAILED");
        return false;
    }
    Serial.printf("[prov] AP-only ch%d, tx=%d, autoReconnect=off, modemSleep=on\n",
                  PROV_AP_CHANNEL, (int)WiFi.getTxPower());

#ifdef PROV_NO_HTTP
    // DIAGNOSTIC BUILD: bring the AP up but run NO web server at all. If the crash still
    // happens when a client associates, the server is exonerated and the fault is purely
    // in the WiFi driver's RX path (which the backtrace already suggests: it ends in
    // wDev_ProcessRxSucData -> wifi_malloc -> tlsf_malloc).
    Serial.println("[prov] PROV_NO_HTTP: web server DISABLED (diagnostic build)");
#else
    if (!g_server) {
        g_server = new WebServer(80);
        if (g_server) {
            // HTTP_GET MUST BE EXPLICIT, and this is a genuine trap rather than a style
            // preference. `WebServer::on(uri, handler)` registers with **HTTP_ANY**, not
            // HTTP_GET, and the dispatch loop returns the FIRST handler that matches:
            //
            //     for (handler = _firstHandler; handler; handler = handler->next())
            //       if (handler->canHandle(_currentMethod, _currentUri)) break;
            //
            // so an HTTP_ANY handler registered for "/" swallows POST / as well, and the
            // POST handler below is never reached. That is not theoretical: it is what
            // made "Save to the Board" appear to do nothing — the submit really did
            // arrive, but handleRoot() handled it and re-rendered the form from the
            // STORED values, which looks exactly like a page refresh.
            //
            // The captive-portal probes below are deliberately left as HTTP_ANY: phones
            // send them as GET, and "answer this URI whatever the method" is what we
            // want for a redirect we never inspect.
            g_server->on("/", HTTP_GET, handleRoot);
            g_server->on("/", HTTP_POST, handlePost);
            // Captive-portal probes, answered with a redirect so the OS offers the page.
            // Apple, Android and Windows each use a different URL; without these the
            // phone joins the AP and then appears to do nothing.
            g_server->on("/hotspot-detect.html", handleProbe);      // Apple
            g_server->on("/generate_204", handleProbe);             // Android
            g_server->on("/gen_204", handleProbe);                  // Android (alt)
            g_server->on("/connecttest.txt", handleProbe);          // Windows
            g_server->on("/redirect", handleProbe);                 // generic
            g_server->onNotFound(handleNotFound);
            g_server->begin();
        }
    }
#endif  // PROV_NO_HTTP
    encodeQr();
    g_active = true;

    Serial.printf("[prov] Setup AP '%s' up (WPA2), ip=%s, page %s\n",
                  PROV_AP_SSID, WiFi.softAPIP().toString().c_str(), PROV_AP_URL);
    Serial.println("[prov] usage/weather polling SUSPENDED while Provisioning runs");
    return true;
}

void provStop() {
    if (!g_active) return;
    if (g_server) g_server->stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    // UNDO the station-affecting settings provStart() changed, or "leave Provisioning"
    // would not restore normal operation:
    //   - modem sleep was turned ON to throttle the AP's RX path (see provStart)
    //   - TX power was raised to 17 dBm for the AP's beacons and WPA2 handshake
    // Both are STATION properties too. Leaving them set after Provisioning means the
    // Board's normal poll runs slower and louder than it should, invisibly, for the
    // rest of its life until the next reboot.
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_13dBm);   // the value wifiInit() uses (see data.cpp)
    WiFi.setAutoReconnect(true);
    WiFi.reconnect();
    g_active = false;
    g_qrSize = 0;
    Serial.println("[prov] Setup AP down; modem sleep off, tx restored; normal operation resumes");
}

bool provActive() { return g_active; }

void provTick() {
    if (!g_active || !g_server) return;
    g_server->handleClient();
    // Log association and every HTTP request. This is the ONLY way to tell "the phone
    // never joined the AP" apart from "it joined and asked for nothing" apart from
    // "it asked and got an answer it could not use" — three different problems that
    // all look identical from the operator's side of the glass.
    static int lastStations = -1;
    int st = (int)WiFi.softAPgetStationNum();
    if (st != lastStations) {
        Serial.printf("[prov] stations -> %d\n", st);
        lastStations = st;
    }
    // STACK WATCHDOG. handleClient() runs wherever the Arduino entry point runs, and
    // that is NOT the same task in both environments: the plain-Arduino build runs
    // loop() on its own loopTask (which is why ARDUINO_LOOP_STACK_SIZE=16384 is in
    // platformio.ini), while esp32-arduino-as-IDF runs setup()/loop() on ESP-IDF's
    // "main" task. Reporting uxTaskGetStackHighWaterMark(nullptr) therefore measures
    // whichever one this build uses, which is the one that actually matters here. It is
    // printed while a client is attached so a regression shows up as a number rather
    // than as a panic.
    if (st > 0) {
        static unsigned long last = 0;
        unsigned long now = millis();
        if (now - last > 3000) {
            last = now;
            UBaseType_t hw = uxTaskGetStackHighWaterMark(nullptr);   // words, this task
            Serial.printf("[prov] loop stack free (min ever): %u bytes\n",
                          (unsigned)(hw * sizeof(StackType_t)));
        }
    }

    // The resolve grace window (ticket #9): the location was resolved and saved,
    // and the page is still being served so the phone's auto-refresh picks up the
    // echo. Only when the window ends does the Board commit — flag, AP down,
    // reboot — i.e. the operator saw what was understood before it took effect.
    if (g_verifyState == PROV_VERIFY_OK && g_resolveGraceUntil &&
        (long)(millis() - g_resolveGraceUntil) >= 0) {
        g_resolveGraceUntil = 0;
        Serial.println("[prov] resolve OK — marking Provisioned");
        provCommitAndReboot();
        return;
    }

    // The verify phase's state machine (ticket #8). provverify.cpp is host-tested;
    // this is where it meets the real clock and the real WiFi.status().
    if (g_verifyState == PROV_VERIFY_RUNNING) {
        prov_verify_fail_t fail = PROV_VERIFY_FAIL_NONE;
        prov_verify_state_t next = provVerifyPoll(g_verifyState, g_verifyStart, millis(),
                                                 PROV_VERIFY_BUDGET_MS, (int)WiFi.status(), &fail);
        if (next != PROV_VERIFY_RUNNING) {
            g_verifyState = next;
            if (next == PROV_VERIFY_OK) {
                const char* loc = cfgWeatherLocation();
                if (!loc || !*loc || WEATHER_API_KEY[0] == '\0') {
                    // No weather to resolve: a blank location is "no weather wanted",
                    // not a failure, and a build without a key cannot resolve at
                    // all. Commit exactly as #8 did — THE writer of the Provisioned
                    // flag; the invariant that makes write-first safe (Amendment 4).
                    Serial.printf("[prov] verify OK — associated with '%s' in %lu s; marking Provisioned\n",
                                  g_verifySsid, (unsigned long)(provVerifyElapsed(g_verifyStart, millis()) / 1000));
                    provCommitAndReboot();
                    return;
                }
                // The station is connected — the one moment the Board can reach the
                // weather service without the operator's network being down (ticket
                // #9). ONE search call; the outcome decides what happens next.
                char name[64], where[80];
                prov_resolve_outcome_t r =
                    provResolveAttempt(loc, name, sizeof name, where, sizeof where);
                if (r == PROV_RESOLVE_OK) {
                    cfgSetWeatherLocation(name);
                    cfgSaveWeather();
                    snprintf(g_resolveEcho, sizeof g_resolveEcho,
                             "Weather resolved: %s — the Board is rebooting onto your network now.",
                             where);
                    g_resolveGraceUntil = millis() + kResolveGraceMs;
                    Serial.printf("[prov] verify OK — associated with '%s'; weather '%s' resolved to '%s' (echoing on the page; reboot in %u s)\n",
                                  g_verifySsid, loc, name, (unsigned)(kResolveGraceMs / 1000));
                    return;   // the commit is deferred to the grace window above
                }
                // A resolve failure is a survivable failure: the AP comes back on
                // channel 1, the written values stay, the flag is NOT set, and the
                // operator corrects and Saves again. The two failures stay distinct
                // on every surface — the place was not found vs the service could
                // not be reached.
                if (r == PROV_RESOLVE_NO_MATCH) {
                    snprintf(g_fail.notice, sizeof g_fail.notice,
                             "Resolve: no place by that name was found — check the spelling and Save again.");
                    snprintf(g_fail.reason, sizeof g_fail.reason, "no such place");
                } else {
                    snprintf(g_fail.notice, sizeof g_fail.notice,
                             "Resolve: the service could not be reached — Save again to try once more.");
                    snprintf(g_fail.reason, sizeof g_fail.reason, "weather service unreachable");
                }
                g_verifyState = PROV_VERIFY_FAILED;
                provRestoreApOnly("resolve", g_fail.reason);
                return;
            }
            g_verifyFail = fail;
            snprintf(g_fail.notice, sizeof g_fail.notice,
                     "Connection failed (%s) — correct the form and Save again.",
                     provVerifyReasonText(fail));
            snprintf(g_fail.reason, sizeof g_fail.reason, "%s", provVerifyReasonText(fail));
            provRestoreApOnly("verify", g_fail.reason);
        }
    }
}

// ── persistence ──────────────────────────────────────────────────────────────
// Provisioning is a DELIBERATE state, so it survives a reboot. Without this, a power
// blip, a cable nudge, or any tool that hard-resets the chip takes the Panel screen
// and the operator has to re-enter it — taking the QR away exactly when someone is
// trying to focus a camera on it. (That happened during development, repeatedly.)
//
// Stored in the same NVS namespace the config uses, so FACTORY clears it too.
static const char* kProvKey = "prov";

void provSaveFlag(bool on) {
#ifndef CFG_HOST_TEST
    Preferences p;
    p.begin("llmtick", false);
    p.putBool(kProvKey, on);
    p.end();
#endif
}

// Read the flag at boot and re-enter Provisioning if it was left active.
void provRestoreIfSaved() {
#ifndef CFG_HOST_TEST
    Preferences p;
    p.begin("llmtick", true);
    bool was = p.getBool(kProvKey, false);
    p.end();
    if (was) {
        Serial.println("[prov] resuming Provisioning from before the reboot");
        provEnter();
    }
#endif
}

void provEnterIfNeverProvisioned() {
#ifndef CFG_HOST_TEST
    if (g_active) return;   // already restoring a deliberate Provisioning
    // The Provisioned flag is authoritative (ADR-0001 Amendment 4): set only by a
    // successful verify (or a deliberate submit that left no network to verify), it
    // is the one fact that moves a Board out of this boot behaviour. A Board that
    // has been through Provisioning never auto-enters it again — including one whose
    // SSID was later deliberately cleared, which is "no network" and must NOT
    // resurrect the Setup AP. (cfgWifiIsStored() alone cannot tell that case from a
    // Factory-fresh Board; the flag is what can.)
    if (provIsProvisioned()) return;
    // Legacy: a Board written over serial (SETWIFI) before the verify phase existed
    // has stored credentials and no flag. Stored credentials stand as provisioned.
    if (cfgWifiIsStored()) return;

    Serial.println("[prov] no WiFi credentials have ever been stored — offering Provisioning");
    Serial.println("[prov] (a network failure on an ALREADY-provisioned Board never does this; ADR-0001)");
    provEnter();
#endif
}

void provEnter() {
    if (g_active) { Serial.println("[prov] already in Provisioning"); return; }
    if (!provStart()) { Serial.println("[prov] could not start Provisioning"); return; }
    provSaveFlag(true);
    provLedOn();
    provPrintStatus();
}

void provLeave() {
    if (!g_active) { Serial.println("[prov] not in Provisioning"); return; }
    if (g_verifyState == PROV_VERIFY_RUNNING) {
        // A PROV stop mid-verify cancels the attempt. The written values stay SAVED
        // (write-first, Amd 4) and the flag stays unset — but at the next boot the
        // stored credentials stand: the legacy cfgWifiIsStored() gate re-enters
        // normal operation, so the AP is NOT re-offered and the Board retries the
        // unverified credentials with backoff. Recovery: serial PROV (re-enter
        // Provisioning) or FACTORY (wipe, re-provision from scratch).
        WiFi.disconnect(false, false);
        g_verifyState = PROV_VERIFY_IDLE;
        g_verifyFail = PROV_VERIFY_FAIL_NONE;
        Serial.println("[prov] verify cancelled (PROV stop)");
    } else if (g_verifyState == PROV_VERIFY_OK && g_resolveGraceUntil) {
        // A PROV stop inside the resolve grace window cancels the COMMIT: no flag,
        // no reboot. The resolved location stays SAVED (write-first, Amd 4), and the
        // stored credentials stand as before — Provisioning simply stays up.
        g_resolveGraceUntil = 0;
        g_resolveEcho[0] = '\0';
        g_verifyState = PROV_VERIFY_IDLE;
        WiFi.disconnect(false, false);
        Serial.println("[prov] resolve cancelled (PROV stop) — no flag set; Provisioning stays up");
    }
    provStop();
    provSaveFlag(false);
    showLed(g_scene);          // restore the scene's own colour
}

const char* provApIp() {
    static char ip[16];
    if (!g_active) return "";
    snprintf(ip, sizeof(ip), "%s", WiFi.softAPIP().toString().c_str());
    return ip;
}

void provPrintStatus() {
    if (!g_active) { Serial.println("[prov] inactive"); return; }
    // Report the AP's OWN view of itself, not just our flag: softAPIP() is empty and
    // the station count is meaningless if the interface is not actually up, so this
    // doubles as a self-check that a scan from another machine would agree with.
    IPAddress ip = WiFi.softAPIP();
    // Print the CREDENTIALS AS BYTES. A phone that reads the QR and then fails with
    // "wrong password" means the string the QR carries and the string the AP expects
    // differ — invisible to the eye, obvious as hex. Also reports the AP's own SSID and
    // password from the driver, so a mismatch with the compile-time constants shows up.
    String apSsid = WiFi.softAPSSID();
    Serial.printf("[prov] active: ssid='%s' pass='%s' url=%s qr=%s ap_ip=%s stations=%d\n",
                  PROV_AP_SSID, PROV_AP_PASS, PROV_AP_URL,
                  g_qrSize ? "encoded" : "NOT ENCODED",
                  ip.toString().c_str(), (int)WiFi.softAPgetStationNum());
    Serial.printf("[prov] driver sees SSID '%s' (len %u)\n",
                  apSsid.c_str(), (unsigned)apSsid.length());
    Serial.print("[prov] AP pass bytes : ");
    for (const char* p = PROV_AP_PASS; *p; p++) Serial.printf("%02X ", (unsigned char)*p);
    Serial.println();
    Serial.printf("[prov] QR payload (%u): %s\n",
                  (unsigned)strlen(g_qrPayload), g_qrPayload);
    Serial.print("[prov] QR payload bytes: ");
    for (const char* p = g_qrPayload; *p; p++) Serial.printf("%02X ", (unsigned char)*p);
    Serial.println();
    if (g_verifyState == PROV_VERIFY_RUNNING)
        Serial.printf("[prov] verify running: ssid='%s' (%lu s elapsed)\n",
                      g_verifySsid, (unsigned long)(provVerifyElapsed(g_verifyStart, millis()) / 1000));
    else if (g_verifyState == PROV_VERIFY_OK && g_resolveGraceUntil)
        Serial.printf("[prov] resolve OK: grace window, reboots in %u s\n",
                      (unsigned)((g_resolveGraceUntil - millis()) / 1000));
    else if (g_verifyState == PROV_VERIFY_FAILED)
        Serial.printf("[prov] last verify FAILED (%s); values kept\n", g_fail.reason);
    Serial.printf("[prov] provisioned flag: %s\n", provIsProvisioned() ? "set" : "unset");
}

// ── Panel screen ─────────────────────────────────────────────────────────────
// Everything here writes BYTE-SWAPPED 565 directly, like wxscene.cpp: the framebuffer is
// blitted raw, so a logical colour must be swapped on the way in. The helper is
// sw565(), shared from tick.h.
static inline uint16_t sw(uint16_t logical) { return sw565(logical); }

static void fillRow(uint16_t* buf, int w, int y, uint16_t c) {
    uint16_t* row = buf + (size_t)y * w;
    for (int x = 0; x < w; x++) row[x] = c;
}

static void fillRect(uint16_t* buf, int w, int h, int x0, int y0, int rw, int rh, uint16_t c) {
    for (int y = y0; y < y0 + rh; y++) {
        if (y < 0 || y >= h) continue;
        uint16_t* row = buf + (size_t)y * w;
        for (int x = x0; x < x0 + rw; x++) {
            if (x < 0 || x >= w) continue;
            row[x] = c;
        }
    }
}

// Text is drawn through the sprite (LovyanGFX owns the fonts). Everything else is a
// direct write. `uiSpr` is bound to the active frame by renderTask.
//
// Shrinks to Font2 if a line would not fit the width — the same auto-shrink the usage
// page already uses for wide values. The credentials are the one thing a human must
// read correctly off the glass, so they must never be clipped or run off the edge.
static void drawCentered(int y, uint16_t colour, int font, const char* text) {
    LGFX_Sprite& s = *uiSpr;
    const lgfx::IFont* f = &fonts::Font0;
    if (font == 2)      f = &fonts::Font2;
    else if (font == 4) f = &fonts::Font4;
    s.setFont(f);
    const int avail = SCREEN_W - 8;
    if (font != 0 && s.textWidth(text) > avail) {
        f = &fonts::Font0;          // too wide even at Font2 -> drop to Font0
        s.setFont(f);
    }
    int tw = s.textWidth(text);
    s.setTextColor(colour);
    s.setCursor((SCREEN_W - tw) / 2, y);
    s.print(text);
}

void provRender(uint16_t* buf, int w, int h) {
    const uint16_t bg  = sw(kBgLogical);
    const uint16_t ink = sw(kInkLogical);

    // NO uiSpr->setBuffer() here — the full explanation lives in ui.cpp at
    // renderUiScene(), which is where the call used to be. In one line: `buf` IS
    // uiSpr's own createSprite() buffer, and setBuffer() calls deleteSprite() ->
    // release() -> heap_free() on it, so calling it each frame freed the live
    // framebuffer and left the sprite drawing into dangling memory.

    // FLAT background over the whole panel — no scene, maximum QR contrast.
    //
    // COLOUR PATH: the two constants are the one documented exception to the
    // "pre-shift every UI colour through wb565()" rule (AGENTS.md, HANDOFF item 4).
    // wb565() exists to tame this Panel's green cast; pure white and pure black are
    // chosen here because a QR is a binary code read by contrast ratio, not a
    // brand-coloured surface, and a shifted pair shrinks the margin that margin
    // decides. Greyscale has no cast to remove.
    for (int y = 0; y < h; y++) fillRow(buf, w, y, bg);

    // ── layout ──────────────────────────────────────────────────────────────────
    // Three stacked bands, and the QR's size is NOT fixed: it is whatever version the
    // encoder needed, which depends on the payload length (version 3 / 29x29 for the
    // current SSID+passphrase, but a longer passphrase would need more). So the QR is
    // given the leftover height between the title and the credential block rather than
    // assumed to be 116 px. Assuming it is what broke this screen: when encodeQr()
    // started returning version 3 instead of version 1, the QR grew by 32 px and pushed
    // the OPEN line down onto the PROV STOP line, both at y=302, so they overdrew each
    // other into an unreadable smear.
    //
    // TOP-BAND MEANDER GUARD. Display rows 0-59 must stay one uniform colour on this
    // Panel: the ST7789's last ~60 RAM rows meander in luminance, and a uniform field
    // has no edges for it to modulate (HANDOFF.md item 2). Both scenes obey this. An
    // earlier version of this screen drew its title at y=30, which put dark text inside
    // the guarded band. So every inked row below is >= kTopSafeY.
    static const int kTopSafeY = 60;      // first row free to carry content
    static const int kTitleY    = 62;      // title baseline
    static const int kTitleH    = 22;      // room for the Font4 title beneath it
    static const int kCredGap   = 10;
    static const int kLineStep  = 24;      // Font2 line pitch
    static const int kFooterY   = 300;     // "PROV STOP ..." — the last line, bottom-anchored

    const int qrTop   = kTopSafeY + kTitleH;                   // 82: below title
    const int credTop = kFooterY - (kLineStep * 2 + 14);       // first WIFI/PASS line
    int qrBottom      = credTop - kCredGap;
    int qrAvail       = qrBottom - qrTop;
    if (qrAvail < 40) qrAvail = 40;                            // never degenerate

    int total = (g_qrSize ? g_qrSize : 21) + kQuietModules * 2;
    int scale = qrAvail / total;                               // fit the HEIGHT
    if ((w - 8) / total < scale) scale = (w - 8) / total;       // then the width
    if (scale < 3) scale = 3;

    int qrPx = total * scale;
    int x0 = (w - qrPx) / 2;
    int y0 = qrTop + (qrAvail - qrPx) / 2;                     // centre in the band

    if (g_qrSize) {
        for (int my = 0; my < g_qrSize; my++) {
            for (int mx = 0; mx < g_qrSize; mx++) {
                // Dark modules are "on". qrcode_getModule returns true for a dark module.
                uint16_t c = qrcode_getModule(&g_qr, (uint8_t)mx, (uint8_t)my) ? ink : bg;
                int px = x0 + (mx + kQuietModules) * scale;
                int py = y0 + (my + kQuietModules) * scale;
                fillRect(buf, w, h, px, py, scale, scale, c);
            }
        }
    }

    // Title above the QR, credentials below it. Every y here is >= kTopSafeY, and the
    // three credential lines occupy their own precomputed band so they can never land
    // on the footer or on each other, whatever version the encoder picked.
    drawCentered(kTitleY, ink, 4, "PROVISIONING");

    char line[64];
    snprintf(line, sizeof(line), "WIFI  %s", PROV_AP_SSID);
    drawCentered(credTop, ink, 2, line);
    snprintf(line, sizeof(line), "PASS  %s", PROV_AP_PASS);
    drawCentered(credTop + kLineStep, ink, 2, line);

    const char* ip = provApIp();
    snprintf(line, sizeof(line), "OPEN  http://%s/", (ip && *ip) ? ip : "192.168.4.1");
    drawCentered(credTop + kLineStep * 2, ink, 2, line);

    // Footer: normally the exit hint. While the verify phase runs it carries the
    // phase's status instead — the Panel is what anyone else in the room can see
    // (Amendment 3), and the status is what they need. The credential lines above
    // (and the QR) stay, so the AP remains joinable either way.
    const char* footer = "PROV STOP  to leave";
    if (g_verifyState == PROV_VERIFY_RUNNING) {
        uint32_t leftMs = PROV_VERIFY_BUDGET_MS - provVerifyElapsed(g_verifyStart, millis());
        if (leftMs > PROV_VERIFY_BUDGET_MS) leftMs = 0;   // clamp a late frame past the budget (wrap-safe)
        snprintf(line, sizeof(line), "VERIFYING  %lus left", (unsigned long)(leftMs / 1000));
        footer = line;
    } else if (g_verifyState == PROV_VERIFY_FAILED) {
        // g_fail.reason holds the short reason for whichever phase failed — the verify
        // reason, or "no such place" / "weather service unreachable" for a resolve
        // failure, where g_verifyFail is FAIL_NONE and would render blank.
        snprintf(line, sizeof(line), "FAILED  %s", g_fail.reason);
        footer = line;
    }
    drawCentered(kFooterY, ink, 0, footer);
}
