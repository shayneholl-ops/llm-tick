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
    PROV_FORM_ERR_SERVER_HOST,         // empty, over-long, or bad characters
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
// IMPORTANT — the returned pointers are OWNED BY THE CONFIG SEAM, not by this function,
// and are valid until the next cfg* setter. Do not cache them across a submit. (An
// earlier draft returned the Server IP through a local buffer that died with the frame;
// the dangling pointer survived review-by-eye because snprintf had already written a
// correct-looking "10.0.0.7" into it, and the stack garbage only showed up as trailing
// bytes when the host test printed the length.)
prov_form_values_t provFormCurrentValues();

// Render the whole page, pre-filled from `v`.
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
// Submits are GROUPED, which is what makes per-field fallback work: a submit whose Server
// fields are all blank leaves WiFi untouched, and vice versa. Within a group a blank field
// is stored as EXPLICITLY EMPTY, not as "forget my Factory default" — the unset-vs-empty
// rule, and the reason this is separate from provValidate.
//
// Returns false and stores nothing when validation fails.
bool provApply(const prov_form_values_t& v, prov_form_error_t* err);