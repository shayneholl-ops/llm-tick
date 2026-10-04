// provform.h — the provisioning FORM, as pure logic.
//
// WHY THIS IS ITS OWN FILE
// The three rules that make the form correct are all pure logic and all have been wrong
// before:
//
//   1. pre-fill shows what is EFFECTIVE, while "unset" and "explicitly empty" stay
//      distinguishable (CONTEXT.md — conflating them is the bug this seam exists to
//      avoid),
//   2. the passphrase never reaches the page,
//   3. a refused submit stores nothing at all.
//
// None of that needs a Board, a radio, or a web server. Keeping it here — with no Arduino
// includes, exactly as config.h does — is what lets tools/provformtest exercise all three
// in seconds rather than a 20-minute flash cycle. prov.cpp keeps only the WebServer
// wiring: parse into a prov_form_values_t, call the functions below, render the result.
//
// C++ (no extern "C"): provBuildForm() returns a std::string by value. A char* out-param
// would mean either a leak or a hand-sized buffer for a ~2 KB page, and the one thing
// this module must never do is truncate a form an operator is about to submit.
#pragma once

#include <string>

// The five submitted values, plus what to pre-fill.
//
// A NULL `const char*` is treated as "". One shape is used for pre-filling, validating
// and applying, so the same five values cannot be transposed between them.
struct prov_form_values_t {
    const char* ssid;      // WiFi SSID
    const char* pass;      // WiFi passphrase — NEVER rendered back
    const char* host;      // Server hostname
    const char* ip;        // Server static IP, optional; "" means "no static fallback"
    const char* weather;   // weather location, optional
};

// Why a submit was refused. PROV_FORM_OK is the only value meaning "store it".
//
// These are distinct rather than one opaque BAD_INPUT because the operator has to be told
// WHICH field was wrong — "invalid input" sends them hunting through five boxes.
enum prov_form_error_t {
    PROV_FORM_OK = 0,
    PROV_FORM_ERR_SSID_TOO_LONG,       // SSID over 32 bytes (the 802.11 limit)
    PROV_FORM_ERR_PASS_TOO_LONG,       // WPA2 passphrase over 63 bytes
    PROV_FORM_ERR_SERVER_HOST,         // hostname over 63 bytes
    PROV_FORM_ERR_SERVER_IP,           // present but not four dotted octets 0-255
    PROV_FORM_ERR_WEATHER_TOO_LONG,
};

// A short, operator-facing message for `err`. Never NULL, never echoes anything the
// operator typed, and safe to put straight into the page.
const char* provFormErrorText(prov_form_error_t err);

// Read the values currently in effect, for pre-filling the form.
//
// An UNSET field yields its Factory default; a field explicitly stored as EMPTY yields
// "". That distinction is the whole point of the seam and is why this reads through
// cfgServerIpState() rather than just formatting the effective address.
//
// The returned `pass` is deliberately EMPTY — the current passphrase is never handed to
// anything that could render it.
//
// IMPORTANT — the returned pointers are OWNED BY THE CALLEE, not by this function.
//
//   ssid, host, weather   point into the config seam's storage; valid until the next
//                         cfg* setter.
//   ip                    points at a static buffer INSIDE provFormCurrentValues(); valid
//                         only until the next call to THIS function. Do not hold it.
//
// Both lifetimes are shorter than they look, which is why provBuildForm() takes the
// struct by const reference and renders immediately. An earlier draft formatted the IP
// into a local buffer and returned it; the dangling pointer survived review-by-eye
// because snprintf had already written a correct-looking "10.0.0.7" into it, and only
// the stack bytes after it were garbage.
prov_form_values_t provFormCurrentValues();

// Render the whole page, pre-filled from `v`.
//
// `v` is also rendered as hidden fields so a submit can be compared against what the
// operator was actually shown — see provApply() for why that distinction is load-bearing.
//
// `notice` (NULL or "" for none) is a status line above the form — a refusal reason, or
// a confirmation. `isError` only styles it.
//
// The passphrase is NEVER rendered, whatever `v.pass` holds: its input is always empty,
// with a placeholder explaining that a blank means "leave the current one alone". So
// there is no path by which the value reaches the page source, and no exception to get
// wrong later.
std::string provBuildForm(const prov_form_values_t& v,
                          const char* notice = nullptr,
                          bool isError = false);

// Check a submit WITHOUT storing anything. Returns true when acceptable; on false,
// *err says which field was at fault.
//
// A blank Server IP is VALID and means "no static fallback" — deliberately NOT "fall back
// to the Factory default", which would silently point the Board at the wrong machine. A
// blank weather location is likewise valid and means "no weather".
bool provValidate(const prov_form_values_t& v, prov_form_error_t* err);

// Validate, and if acceptable, write through the Config seam and persist.
//
// WHY THIS TAKES AN "ORIGINALLY SHOWN" SNAPSHOT
// Per-field fallback cannot be done by looking at which fields arrived blank. The form
// PRE-FILLS every field except the passphrase, so a WiFi-only submit ARRIVES carrying the
// Server's current value — which, on a Board that has never been provisioned, is the
// Factory default. Storing that would freeze today's Factory default into NVS as if the
// operator had chosen it, and a later secrets.h edit would then be silently ignored. That
// is precisely the harm cfgSaveWifi()'s own comment warns against.
//
// So the form carries what it originally displayed in hidden fields, and this function
// stores ONLY the fields that differ from it. An operator who changes one field gets one
// field stored; every other field keeps the state it had, including "unset".
//
// The passphrase is exempt: it is never pre-filled, so it cannot be compared, and a blank
// one means "keep the stored password" — never "store an empty password", which would
// leave the Board unable to join any secured network.
//
// Returns false and stores nothing when validation fails.
bool provApply(const prov_form_values_t& submitted,
               const prov_form_values_t& originallyShown,
               prov_form_error_t* err = nullptr);
