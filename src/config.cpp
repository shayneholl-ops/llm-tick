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

// Return the runtime value if set, else the Factory default. Never NULL.
static const char* resolve(const OptStr& f, const char* def) {
    return f.set ? f.v : def;
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
const char* cfgWifiSsid(void) { return resolve(g_ssid, CFG_DEF_WIFI_SSID); }
const char* cfgWifiPass(void) { return resolve(g_pass, CFG_DEF_WIFI_PASS); }
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
    const char* text = g_ip.set ? g_ip.v : CFG_DEF_SERVER_IP;
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
