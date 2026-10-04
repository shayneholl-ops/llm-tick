// config.cpp — the Config seam's storage. See config.h for why it exists.
//
// Storage is a plain in-RAM mirror today (every field starts unset, so every getter
// returns its Factory default). Ticket #2 adds the NVS backing store and turns these
// setters into persisting writes; keeping the storage behind these functions is the
// whole point of the seam, so that change stays local to this file.
#ifdef CFG_HOST_TEST
// The host test supplies -DCFG_DEF_* on the command line and has no secrets.h.
#else
#include "config_defaults.h"   // brings the Factory defaults in from secrets.h
#endif

#include "config.h"

#include <string.h>

// The Factory defaults must be present by now (config_defaults.h or the host test's
// -D flags). Guarding here rather than in config.h keeps the public header usable by
// consumers that never touch the defaults.
#ifndef CFG_DEF_WIFI_SSID
#error "CFG_DEF_WIFI_SSID must be defined (config_defaults.h, or -DCFG_DEF_* in the host test)"
#endif
#ifndef CFG_DEF_WIFI_PASS
#error "CFG_DEF_WIFI_PASS must be defined"
#endif
#ifndef CFG_DEF_SERVER_HOST
#error "CFG_DEF_SERVER_HOST must be defined"
#endif
#ifndef CFG_DEF_SERVER_IP
#error "CFG_DEF_SERVER_IP must be defined (dotted-quad or comma-separated, text form)"
#endif
#ifndef CFG_DEF_SERVER_PORT
#error "CFG_DEF_SERVER_PORT must be defined"
#endif
#ifndef CFG_DEF_WEATHER_LOCATION
#error "CFG_DEF_WEATHER_LOCATION must be defined"
#endif

// A field is either "never set" (fall back to the Factory default) or set to a
// value that may legitimately be empty. `set` is what distinguishes the two.
namespace {
constexpr int kMaxLen = 64;

struct OptStr {
    bool set = false;
    char v[kMaxLen] = "";
};

OptStr g_ssid, g_pass, g_host, g_loc;
OptStr g_ip;                       // dotted-quad text; "" set explicitly = no fallback
unsigned short g_port = 0;         // 0 == unset
}  // namespace

// Copy `src` (NULL-safe) into `dst`, truncating rather than overflowing.
static void store(OptStr& dst, const char* src) {
    dst.set = true;
    if (!src) { dst.v[0] = 0; return; }
    strncpy(dst.v, src, kMaxLen - 1);
    dst.v[kMaxLen - 1] = 0;
}

// The two Factory defaults the freeze test rotates are held in VARIABLES rather than
// used as macros directly, so the test can change what "unset" resolves to without any
// pointer-identity trickery (comparing string literals by pointer is unspecified
// behaviour, and the compiler says so). In the firmware they are simply initialised
// from the compile-time macros and never moved.
static const char* g_defPass = CFG_DEF_WIFI_PASS;
static const char* g_defIp   = CFG_DEF_SERVER_IP;

// Return the runtime value if set, else the Factory default. Never NULL.
static const char* resolve(const OptStr& f, const char* def) {
    return f.set ? f.v : def;
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
const char* cfgWifiSsid(void) { return resolve(g_ssid, CFG_DEF_WIFI_SSID); }
const char* cfgWifiPass(void) { return resolve(g_pass, g_defPass); }
void cfgSetWifiSsid(const char* v) { store(g_ssid, v); }
void cfgSetWifiPass(const char* v) { store(g_pass, v); }
void cfgClearWifiSsid(void) { g_ssid = OptStr(); }
void cfgClearWifiPass(void) { g_pass = OptStr(); }

// ── Server ───────────────────────────────────────────────────────────────────
const char* cfgServerHost(void) { return resolve(g_host, CFG_DEF_SERVER_HOST); }
void cfgSetServerHost(const char* v) { store(g_host, v); }
void cfgClearServerHost(void) { g_host = OptStr(); }

cfg_ip_state_t cfgServerIpState(void) {
    if (!g_ip.set) return CFG_IP_UNSET;
    return g_ip.v[0] != 0 ? CFG_IP_SET : CFG_IP_EMPTY;
}

bool cfgServerIpIsSet(void) {
    // Unset -> the Factory default IP applies, which IS a usable fallback.
    // Empty -> explicitly no fallback.
    return cfgServerIpState() != CFG_IP_EMPTY;
}

void cfgServerIpOctets(unsigned char out[4]) {
    // Accepts "192, 168, 1, 69" (how CFG_STR leaves the secrets.h macro), "192.168.1.69",
    // and any mix. Whitespace is skipped; out-of-range octets are clamped.
    const char* text = g_ip.set ? g_ip.v : g_defIp;
    int idx = 0, val = 0;
    bool any = false;
    for (const char* p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (val > 255) val = 255;
            any = true;
            continue;
        }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;  // tolerate spacing
        // Separator (',' or '.'), or the terminator.
        if (idx < 4) out[idx++] = (unsigned char)(any ? val : 0);
        val = 0; any = false;
        if (*p == 0 || idx >= 4) break;
    }
    while (idx < 4) out[idx++] = 0;   // malformed input pads with 0
}

void cfgSetServerIp(const char* dotted) { store(g_ip, dotted); }
void cfgClearServerIp(void) { g_ip = OptStr(); }

unsigned short cfgServerPort(void) { return g_port ? g_port : (unsigned short)CFG_DEF_SERVER_PORT; }
void cfgSetServerPort(unsigned short port) { g_port = port; }

// ── Weather ──────────────────────────────────────────────────────────────────
const char* cfgWeatherLocation(void) { return resolve(g_loc, CFG_DEF_WEATHER_LOCATION); }
void cfgSetWeatherLocation(const char* v) { store(g_loc, v); }
void cfgClearWeatherLocation(void) { g_loc = OptStr(); }

// ── Lifecycle ────────────────────────────────────────────────────────────────
void cfgReset(void) {
    g_ssid = OptStr();
    g_pass = OptStr();
    g_host = OptStr();
    g_ip   = OptStr();
    g_loc  = OptStr();
    g_port = 0;
}

// ── Persistence ──────────────────────────────────────────────────────────────
// NVS access goes through cfgStore* so this translation unit stays free of Arduino
// headers and remains host-testable: the host build links the no-op stubs below and
// exercises every rule about unset-vs-empty without a Board or a flash write.
// NVS keys are limited to 15 characters. Short and stable, and shared by BOTH builds
// so the host test exercises the same key names the firmware stores: renaming one
// orphans the value already saved on a deployed Board.
static constexpr const char* kKeySsid = "ssid";
static constexpr const char* kKeyPass = "pass";
static constexpr const char* kKeyHost = "host";
static constexpr const char* kKeyIp   = "ip";
static constexpr const char* kKeyLoc  = "loc";
static constexpr const char* kKeyPort = "port";

#ifdef CFG_HOST_TEST

// Host build: an in-memory fake store. Keeping the SAME shape as the NVS path means
// the persistence rules (a key exists vs a key holds an empty string, wipe, reload)
// are exercised on the host, which is the only place they can be tested cheaply
// without risking a Board that cannot reconnect.
namespace {
constexpr int kFakeMax = 8;
struct FakeEntry { const char* key; char val[kMaxLen]; bool used; };
FakeEntry g_fake[kFakeMax];

FakeEntry* fakeFind(const char* key) {
    for (int i = 0; i < kFakeMax; i++)
        if (g_fake[i].used && strcmp(g_fake[i].key, key) == 0) return &g_fake[i];
    return nullptr;
}
}  // namespace

static bool storeHas(const char* key) { return fakeFind(key) != nullptr; }

static void storeGet(const char* key, char* out, int outLen) {
    FakeEntry* e = fakeFind(key);
    const char* v = e ? e->val : "";
    strncpy(out, v, outLen - 1);
    out[outLen - 1] = 0;
}

static void storePut(const char* key, const char* v) {
    FakeEntry* e = fakeFind(key);
    if (!e) {
        for (int i = 0; i < kFakeMax; i++)
            if (!g_fake[i].used) { e = &g_fake[i]; e->key = key; e->used = true; break; }
    }
    if (!e) return;   // fake store full; the real one has room
    strncpy(e->val, v ? v : "", kMaxLen - 1);
    e->val[kMaxLen - 1] = 0;
}

static void storePutU16(const char*, unsigned short) {}
static bool storeGetU16(const char*, unsigned short*) { return false; }
static void storeWipe() { for (int i = 0; i < kFakeMax; i++) g_fake[i].used = false; }

// Test-only hooks. The host build cannot reboot, so it RECORDS the request and the
// test asserts on it — that is what makes "FACTORY wipes then reboots, in that order"
// a testable claim rather than a hope.
static bool g_rebootRequested = false;
bool cfgTestRebootRequested(void) { return g_rebootRequested; }
void cfgTestClearReboot(void) { g_rebootRequested = false; }
static void doReboot() { g_rebootRequested = true; }
void cfgPrintReport(void) {}

// Test-only: wipe the fake store without recording a reboot, so a test can set up and
// tear down independently of cfgFactoryReset's reboot request.
void cfgTestWipe(void) { storeWipe(); }

void cfgTestSetFactoryPass(const char* v) { g_defPass = v; }
void cfgTestSetFactoryIp(const char* dotted) { g_defIp = dotted; }

#else

#include <Preferences.h>
#include <Arduino.h>

namespace {
Preferences g_prefs;
bool g_prefsOpen = false;
constexpr const char* kNamespace = "llmtick";

// A stored value records BOTH the bytes and whether it was set at all, so an empty
// string can be told from an absent key. That distinction is the whole feature.
void open() {
    if (!g_prefsOpen) { g_prefs.begin(kNamespace, false); g_prefsOpen = true; }
}
}  // namespace

static bool storeHas(const char* key) { open(); return g_prefs.isKey(key); }

static void storeGet(const char* key, char* out, int outLen) {
    open();
    // An absent key reads back as "" — callers gate on storeHas() first, so that is
    // only ever reached for a key known to exist.
    String v = g_prefs.getString(key, "");
    strncpy(out, v.c_str(), outLen - 1);
    out[outLen - 1] = 0;
}

static void storePut(const char* key, const char* v) { open(); g_prefs.putString(key, v ? v : ""); }
static void storePutU16(const char* key, unsigned short v) { open(); g_prefs.putUShort(key, v); }

static bool storeGetU16(const char* key, unsigned short* out) {
    open();
    if (!g_prefs.isKey(key)) return false;
    *out = g_prefs.getUShort(key, 0);
    return true;
}

static void storeWipe() { open(); g_prefs.clear(); }

static void doReboot() { ESP.restart(); }

#endif  // CFG_HOST_TEST

// Load every field that was stored. An absent key leaves the field UNSET, so the
// Factory default applies; a key present but empty loads as EMPTY, so it does not.
void cfgLoad(void) {
    char tmp[kMaxLen];

    if (storeHas(kKeySsid)) { storeGet(kKeySsid, tmp, kMaxLen); store(g_ssid, tmp); }
    if (storeHas(kKeyPass)) { storeGet(kKeyPass, tmp, kMaxLen); store(g_pass, tmp); }
    if (storeHas(kKeyHost)) { storeGet(kKeyHost, tmp, kMaxLen); store(g_host, tmp); }
    if (storeHas(kKeyIp))   { storeGet(kKeyIp,   tmp, kMaxLen); store(g_ip,   tmp); }
    if (storeHas(kKeyLoc))  { storeGet(kKeyLoc,  tmp, kMaxLen); store(g_loc,  tmp); }

    unsigned short port = 0;
    if (storeGetU16(kKeyPort, &port)) g_port = port;
}

void cfgSaveWifi(void) {
    // Persist ONLY what was actually set. Storing the RESOLVED value would write
    // today's Factory default into NVS as if it were a deliberate choice, which breaks
    // the per-field rule twice over: a later change to secrets.h would be ignored, and
    // changing one field would silently pin its neighbour.
    storePut(kKeySsid, cfgWifiSsid());
    if (g_pass.set) storePut(kKeyPass, g_pass.v);
}

void cfgSaveServer(void) {
    // The same rule as cfgSaveWifi above, and it was being broken here: writing the
    // RESOLVED hostname would pin today's Factory default into the store even when the
    // caller never touched the field. The symptom was invisible in RAM and only appeared
    // at the next boot — a WiFi-only save left the hostname reading UNSET until cfgLoad()
    // ran, then reported it STORED, and a later secrets.h edit was silently ignored.
    // (Found by a code review of the provisioning form, ticket #7.)
    if (g_host.set) storePut(kKeyHost, g_host.v);
    // The raw state, not the resolved value: an explicitly-empty IP must persist AS
    // empty (meaning "no fallback"), while an unset one must stay unset.
    if (g_ip.set) storePut(kKeyIp, g_ip.v);
    if (g_port) storePutU16(kKeyPort, g_port);
}

void cfgSaveWeather(void) {
    // Same rule: never freeze the Factory default into the store for a field the caller
    // did not set.
    if (g_loc.set) storePut(kKeyLoc, g_loc.v);
}

// Setters that do NOT persist on their own — cfgSave* is the single write point, so a
// caller cannot half-commit a change by setting a field and forgetting to save it.
// Documented here because the split is deliberate, not an oversight.

bool cfgWifiIsStored(void)      { return g_ssid.set; }
bool cfgServerHostIsStored(void){ return g_host.set; }
bool cfgWeatherIsStored(void)   { return g_loc.set; }
bool cfgServerPortIsStored(void){ return g_port != 0; }

void cfgFactoryReset(void) {
    storeWipe();
    cfgReset();
    doReboot();
}

#ifndef CFG_HOST_TEST
// The report needs printf/Serial; the host build omits it rather than faking output.
void cfgPrintReport(void) {
    const char* srcSsid = g_ssid.set ? "stored" : "factory";
    const char* srcHost = g_host.set ? "stored" : "factory";
    const char* srcLoc  = g_loc.set  ? "stored" : "factory";
    const char* ipSrc;
    switch (cfgServerIpState()) {
        case CFG_IP_SET:   ipSrc = "stored"; break;
        case CFG_IP_EMPTY: ipSrc = "none (no fallback)"; break;
        default:           ipSrc = "factory"; break;
    }
    Serial.println("[cfg] effective configuration:");
    Serial.printf("[cfg]   wifi ssid : '%s'   [%s]\n", cfgWifiSsid(), srcSsid);
    // Never print the password. It would land in logs, screenshots and bug reports.
    Serial.printf("[cfg]   wifi pass : %s   [%s]\n",
                  g_pass.set ? (g_pass.v[0] ? "(set, hidden)" : "(set to empty)") : "(factory, hidden)",
                  srcSsid);
    Serial.printf("[cfg]   server    : '%s'   [%s]\n", cfgServerHost(), srcHost);
    unsigned char oct[4];
    cfgServerIpOctets(oct);
    Serial.printf("[cfg]   server ip : %u.%u.%u.%u   [%s]\n", oct[0], oct[1], oct[2], oct[3], ipSrc);
    Serial.printf("[cfg]   server port: %u\n", (unsigned)cfgServerPort());
    Serial.printf("[cfg]   weather   : '%s'   [%s]\n", cfgWeatherLocation(), srcLoc);
}
#endif



