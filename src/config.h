// config.h — the Board's runtime configuration, behind one seam.
//
// WHY THIS EXISTS
// The Board's config (WiFi credentials, the Server's hostname and optional static
// IP, the weather location) used to be read as compile-time macros in scattered
// places: wifiInit() and resolveServer() in data.cpp, and WeatherAPI::fetch() in
// weather_api.cpp. With no seam between "where a value comes from" and "what code
// consumes it", adding a runtime source (NVS, written by Provisioning) would have
// meant a search-and-replace through the boot path — the code most able to leave
// the Board unreachable.
//
// THE RULE THAT MATTERS
// A field that was NEVER SET falls back to the Factory default. A field explicitly
// SET TO EMPTY does not: it means "no value", and it stays empty. Collapsing those
// two states is the bug F2 has to avoid — provisioning WiFi alone must not force
// retyping a Server address, and an explicitly blank Server IP must mean "no static
// fallback" rather than "silently use a compile-time IP that may be the wrong box".
//
// Today every field is unset, so every getter returns its Factory default and the
// Board behaves exactly as it did before this file existed. Ticket #2 gives these
// setters an NVS backing store; ticket #1 (this) only builds the seam.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Factory defaults ─────────────────────────────────────────────────────────
// In the firmware these come from secrets.h; the host test build overrides them with
// -DCFG_DEF_*. Both are supplied via config_defaults.h, which only config.cpp
// includes — consumers of this header (data.cpp, weather_api.cpp) never see them and
// never need to. The "must be defined" guards therefore live in config.cpp, where the
// defaults are actually consumed.


// ── WiFi ─────────────────────────────────────────────────────────────────────
// Never NULL: falls back to the Factory default when unset.
const char* cfgWifiSsid(void);
const char* cfgWifiPass(void);
void        cfgSetWifiSsid(const char* v);
void        cfgSetWifiPass(const char* v);
void        cfgClearWifiSsid(void);
void        cfgClearWifiPass(void);

// ── Server ───────────────────────────────────────────────────────────────────
// The hostname is what mDNS resolves; the static IP is the optional fallback.
const char* cfgServerHost(void);
void        cfgSetServerHost(const char* v);
void        cfgClearServerHost(void);

// The static IP has THREE states, and conflating any two of them is the bug this
// feature has to avoid, so the seam exposes the raw state separately from the policy:
//
//   unset   -> no value was ever stored; the Factory default IP applies
//   empty   -> explicitly cleared; there is deliberately NO fallback address
//   set     -> use the given address
//
// cfgServerIpState() answers "what is stored?"; cfgServerIpIsSet() answers the
// policy question "is there a fallback address to use?" (unset -> yes, via the
// Factory default; empty -> no). Ticket #2 needs the raw state to decide whether a
// stored value exists at all, which is why both are exposed.
typedef enum {
    CFG_IP_UNSET = 0,   // never stored -> the Factory default applies
    CFG_IP_EMPTY,       // stored as empty -> no fallback
    CFG_IP_SET,         // stored with an address
} cfg_ip_state_t;

cfg_ip_state_t cfgServerIpState(void);
// True when there IS a usable fallback address (CFG_IP_UNSET or CFG_IP_SET).
bool           cfgServerIpIsSet(void);
// The effective address: the stored one, or the Factory default when unset.
void           cfgServerIpOctets(unsigned char out[4]);
void           cfgSetServerIp(const char* dotted);   // "" == explicitly no fallback
void           cfgClearServerIp(void);

unsigned short cfgServerPort(void);
void           cfgSetServerPort(unsigned short port);

// ── Weather ──────────────────────────────────────────────────────────────────
const char* cfgWeatherLocation(void);
void        cfgSetWeatherLocation(const char* v);
void        cfgClearWeatherLocation(void);

// ── Lifecycle ────────────────────────────────────────────────────────────────
// Wipe every runtime value IN RAM ONLY, returning all fields to their Factory
// defaults. This does NOT touch the backing store — use cfgFactoryReset() to clear
// stored values (it wipes the store and reboots). cfgLoad() would restore them.
void cfgReset(void);

// ── Persistence (ticket #2) ──────────────────────────────────────────────────
// Load stored values from NVS into RAM. Call ONCE, early in setup(), before anything
// reads a getter. With nothing stored this is a no-op and every getter returns its
// Factory default — which is what makes a fresh Board behave as it always did.
void cfgLoad(void);

// True when this field holds a value that came from the store rather than a Factory
// default. Used for the boot report and cfgPrintReport(); nothing in the fetch path
// needs it, which is the point of the seam.
bool cfgWifiIsStored(void);
bool cfgServerHostIsStored(void);
bool cfgWeatherIsStored(void);
bool cfgServerPortIsStored(void);

// Persist the fields that changed. Saving per GROUP (rather than one call per setter)
// keeps the NVS write count low: flash writes are slow and each one is a chance to
// interrupt the radio. Nothing here reboots the Board — see cfgFactoryReset.
void cfgSaveWifi(void);
void cfgSaveServer(void);
void cfgSaveWeather(void);

// Print the EFFECTIVE configuration, and where each value came from (stored vs
// Factory default vs not set). Never prints the password or the API key: this output
// ends up in logs and screenshots.
void cfgPrintReport(void);

// Recovery path for the FACTORY command: wipe the backing store, then reboot so the
// Board comes up on the Factory defaults. Does not return.
//
// Why reboot rather than reconnect in place: the credentials are read once, at boot,
// and several things derive from them (the resolved Server URL, mDNS, the weather
// client). Re-reading them in place would mean re-running that whole boot path while
// the render task and the WiFi driver are live — on a Board with a documented history
// of WiFi-driver instability under load. A reboot is the boring, reliable answer.
void cfgFactoryReset(void);

#ifdef CFG_HOST_TEST
// Host-test-only hooks. The host build cannot touch NVS or reboot, so it substitutes
// an in-memory store and records reboot requests, letting the persistence and
// recovery rules be tested off-Board. Not compiled into the firmware.
void cfgTestWipe(void);
bool cfgTestRebootRequested(void);
void cfgTestClearReboot(void);
// Change what an UNSET field resolves to. This is how a test proves that a save did
// not freeze the old default: rotate the default between the save and the reload and
// check which one the reload picked up.
void cfgTestSetFactoryPass(const char* v);
void cfgTestSetFactoryIp(const char* dotted);
#endif

#ifdef __cplusplus
}
#endif
