// prov.cpp — the Provisioning state. See prov.h for the rules it implements.
//
// Two halves:
//   1. the Setup AP + the placeholder web page (radio side)
//   2. the Panel screen: a QR on a flat background (drawing side)
//
// The QR matrix is built ONCE, when Provisioning is entered, and cached — rendering it
// per frame would re-run the encoder 79 times a second for a static image.
#include "prov.h"
#include "tick.h"
#include "config.h"

#include "provform.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <string.h>

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

static void sendForm(const char* notice, bool isError) {
    std::string page = provBuildForm(provFormCurrentValues(), notice, isError);
    g_server->send(200, "text/html", page.c_str());
}

static void handleRoot() {
    Serial.printf("[prov] GET /        from %s\n", g_server->client().remoteIP().toString().c_str());
    sendForm(nullptr, false);
}

// POST / — validate, store, and re-render the form.
//
// Deliberately does NOT connect, tear the AP down, or reboot: verify-then-reboot is the
// next slice, and this one is the zero-radio-risk slice. The operator can confirm their
// values persisted by reloading, which is what makes it demoable on its own.
//
// A refused submit re-renders with the operator's OWN values still in the boxes (passed
// back in) rather than the stored ones, so a typo is correctable in place instead of
// being silently discarded — the anti-lockout property the whole feature exists for.
static void handlePost() {
    Serial.printf("[prov] POST /       from %s\n", g_server->client().remoteIP().toString().c_str());

    prov_form_values_t v;
    v.ssid    = g_server->arg("ssid").c_str();
    v.pass    = g_server->arg("pass").c_str();
    v.host    = g_server->arg("host").c_str();
    v.ip      = g_server->arg("ip").c_str();
    v.weather = g_server->arg("weather").c_str();

    prov_form_error_t err = PROV_FORM_OK;
    if (!provApply(v, &err)) {
        Serial.printf("[prov] submit REFUSED (error %d) — nothing stored\n", (int)err);
        sendForm(provFormErrorText(err), true);
        return;
    }

    // The passphrase is never logged: this line reaches a serial log that ends up in
    // screenshots and CI output.
    Serial.printf("[prov] submit saved: ssid='%s' host='%s' ip='%s' weather='%s'\n",
                  v.ssid, v.host, v.ip, v.weather);
    Serial.println("[prov] still in Provisioning — no connection attempted by design");

    // Re-render from the stored config rather than echoing the submission back, so what
    // the operator now sees is what the Board actually holds (including a blank passphrase
    // coming back blank, which is correct).
    sendForm(provFormErrorText(PROV_FORM_OK), false);
}

// The page reads this. Deliberately NOT the passphrase or the API key — this handler is
// reachable by anyone on the Setup AP.
static void handleCfg() {
    Serial.printf("[prov] GET /cfg     from %s\n", g_server->client().remoteIP().toString().c_str());
    char json[256];
    snprintf(json, sizeof(json),
             "{\"ssid\":\"%s\",\"server\":\"%s\",\"ip\":\"%s\",\"weather\":\"%s\"}",
             cfgWifiSsid(), cfgServerHost(),
             cfgServerIpIsSet() ? "(set)" : "(none)",   // never echo the real octets
             cfgWeatherLocation());
    g_server->send(200, "application/json", json);
}

static void handleNotFound() {
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
            g_server->on("/", handleRoot);
            g_server->on("/", HTTP_POST, handlePost);
            g_server->on("/cfg", handleCfg);
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
    // "Never provisioned" == no WiFi SSID has ever been STORED. cfgWifiIsStored() is
    // false both for a Factory-fresh Board and for one whose SSID was explicitly
    // cleared — and an explicitly cleared SSID is a deliberate "I want no network",
    // which must NOT resurrect the setup path. Only the first is handled here: a Board
    // that had credentials and then had them cleared is still provisioned, and
    // cfgWifiIsStored() alone cannot distinguish the two cases. Ticket #4's form will
    // record a separate "provisioned" flag precisely so it can.
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

    drawCentered(kFooterY, ink, 0, "PROV STOP  to leave");
}
