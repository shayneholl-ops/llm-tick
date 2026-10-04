// provform.cpp — see provform.h for why this is a separate, Arduino-free translation unit.
//
// The page is assembled by string CONCATENATION, not snprintf: the only formatted value
// is the Server IP in provFormCurrentValues(), and the five form fields are appended
// through esc(), which cannot overrun because it grows the string. Values reach
// provBuildForm() pre-validated on the POST path, but the GET path renders whatever is
// currently configured with no validation in between — so the renderer is bounded by
// construction rather than by trusting its callers.
#include "provform.h"

#include <cstdio>
#include <cstring>

#include "config.h"

// ── limits ───────────────────────────────────────────────────────────────────
// 32 is the 802.11 SSID limit; 63 is the WPA2 passphrase limit. A form that accepts more
// stores a value the radio cannot use, and the operator only finds out at connect time —
// which is exactly the failure this ticket's follow-on slice is about.
//
// The three config limits are 63, NOT 64, because the seam's own OptStr is
// `char v[64]` and store() truncates at kMaxLen - 1 = 63. A limit of 64 here would accept
// a 64-character weather location and then silently store 63 — the operator would see the
// value they typed come back changed, with no error anywhere. Match the seam exactly.
static const size_t kMaxSsid    = 32;
static const size_t kMaxPass    = 63;
static const size_t kMaxHost    = 63;
static const size_t kMaxWeather = 63;

static const char* nz(const char* s) { return s ? s : ""; }
static size_t      len(const char* s) { return strlen(nz(s)); }

// Escape for an HTML attribute value. The five fields are free text typed by a human, so
// a quote or a "<" is a real possibility rather than a theoretical one, and an unescaped
// one would let the page be broken from the Board itself.
static std::string esc(const char* s) {
    std::string out;
    for (const char* p = nz(s); *p; ++p) {
        switch (*p) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += *p;       break;
        }
    }
    return out;
}

// One hidden input carrying a pre-filled value back with the submit.
static std::string hidden(const char* name, const char* value) {
    std::string s = "<input type=\"hidden\" name=\"";
    s += name;
    s += "\" value=\"";
    s += esc(value);
    s += "\">";
    return s;
}

const char* provFormErrorText(prov_form_error_t err) {
    switch (err) {
        case PROV_FORM_OK:                   return "Saved.";
        case PROV_FORM_ERR_SSID_TOO_LONG:
            return "That WiFi name is too long — 32 characters is the maximum.";
        case PROV_FORM_ERR_PASS_TOO_LONG:
            return "That WiFi password is too long — 63 characters is the maximum.";
        case PROV_FORM_ERR_SERVER_HOST:
            return "The Server name is too long — 63 characters is the maximum.";
        case PROV_FORM_ERR_SERVER_IP:
            return "The Server IP must be four numbers between 0 and 255, like 192.168.1.50. "
                   "Leave it blank for no static address.";
        case PROV_FORM_ERR_WEATHER_TOO_LONG:
            return "That weather location is too long.";
    }
    return "Those values could not be saved.";
}

// The formatted address must outlive this frame, because prov_form_values_t holds bare
// `const char*` and the caller renders them AFTER we return. A local buffer here would be
// a dangling pointer — the failure mode is quiet, because snprintf has by then written a
// perfectly plausible "10.0.0.7" and only the stack bytes after it are garbage.
//
// Sized with margin: "255.255.255.255" is 15 characters, and snprintf must also fit the
// terminator. The check below is a backstop against silently truncating an address an
// operator is being shown; a Board whose stored IP is somehow longer than this logs and
// gets an empty box rather than a mangled one.
static char g_ipText[20] = {0};

prov_form_values_t provFormCurrentValues() {
    prov_form_values_t v;
    v.ssid    = cfgWifiSsid();
    v.pass    = "";                 // never handed to the renderer, by design
    v.host    = cfgServerHost();
    v.weather = cfgWeatherLocation();

    // The Server IP has THREE states and the form must show two of them differently:
    // unset falls back to the Factory default, so pre-fill what the Board would actually
    // use; explicitly empty means "no static fallback", so pre-fill an empty box.
    // Collapsing these would hand the operator a Factory IP they had deliberately cleared.
    if (cfgServerIpState() == CFG_IP_EMPTY) {
        g_ipText[0] = '\0';
    } else {
        unsigned char o[4] = {0, 0, 0, 0};
        cfgServerIpOctets(o);
        const int n = snprintf(g_ipText, sizeof(g_ipText), "%u.%u.%u.%u",
                               o[0], o[1], o[2], o[3]);
        if (n < 0 || (size_t)n >= sizeof(g_ipText)) g_ipText[0] = '\0';
    }
    v.ip = g_ipText;
    return v;
}

std::string provBuildForm(const prov_form_values_t& v,
                          const char* notice, bool isError) {
    std::string p;
    p.reserve(2048);

    p +=
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>llm-tick provisioning</title><style>"
        "body{font-family:system-ui,-apple-system,sans-serif;margin:1rem;line-height:1.4;"
        "max-width:34rem;color:#111}"
        "h1{font-size:1.25rem;margin:0 0 .25rem}"
        "p.lead{margin:0 0 1rem;color:#555;font-size:.9rem}"
        "label{display:block;font-weight:600;margin:.85rem 0 .2rem;font-size:.9rem}"
        "input{width:100%;box-sizing:border-box;padding:.6rem;font-size:1rem;"
        "border:1px solid #bbb;border-radius:6px}"
        "p.hint{margin:.2rem 0 0;color:#666;font-size:.8rem}"
        ".notice{padding:.7rem;border-radius:6px;margin:0 0 1rem;font-size:.9rem}"
        ".err{background:#fde8e8;border:1px solid #c0392b;color:#7a1c12}"
        ".ok{background:#e8f6ed;border:1px solid #2e7d4f;color:#1b4d31}"
        "button{margin-top:1.25rem;width:100%;padding:.75rem;font-size:1rem;"
        "border:0;border-radius:6px;background:#111;color:#fff;font-weight:600}"
        "</style></head><body>";

    p += "<h1>llm-tick provisioning</h1>";
    p += "<p class=\"lead\">Saved on the Board itself. It stays on this network "
         "until you finish &mdash; nothing is connected yet.</p>";

    if (notice && *notice) {
        p += "<p class=\"notice ";
        p += isError ? "err" : "ok";
        p += "\">";
        p += esc(notice);
        p += "</p>";
    }

    p += "<form method=\"POST\" action=\"/\">";

    p += "<label for=\"ssid\">WiFi name</label>"
         "<input id=\"ssid\" name=\"ssid\" value=\"";
    p += esc(v.ssid);
    p += "\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\">";

    // The passphrase input is ALWAYS empty. Blanking it means "keep the current one",
    // which is the only way an operator can change one field without retyping this.
    p += "<label for=\"pass\">WiFi password</label>"
         "<input id=\"pass\" name=\"pass\" type=\"password\" value=\"\" "
         "autocomplete=\"new-password\">"
         "<p class=\"hint\">Leave blank to keep the password already stored, unchanged.</p>";

    p += "<label for=\"host\">Server name</label>"
         "<input id=\"host\" name=\"host\" value=\"";
    p += esc(v.host);
    p += "\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\">"
         "<p class=\"hint\">The hostname the Board looks for, e.g. <code>nas.local</code>.</p>";

    p += "<label for=\"ip\">Server IP <span class=\"hint\">(optional)</span></label>"
         "<input id=\"ip\" name=\"ip\" value=\"";
    p += esc(v.ip);
    p += "\" inputmode=\"decimal\" autocomplete=\"off\" spellcheck=\"false\">"
         "<p class=\"hint\">A static address for the Server. "
         "Leave blank for <strong>no</strong> static address.</p>";

    p += "<label for=\"weather\">Weather location</label>"
         "<input id=\"weather\" name=\"weather\" value=\"";
    p += esc(v.weather);
    p += "\" autocomplete=\"off\" spellcheck=\"false\">"
         "<p class=\"hint\">A place name, e.g. <code>Vancouver</code>. Blank for no weather.</p>";

    p += "<button type=\"submit\">Save to the Board</button>";

    // WHAT THE OPERATOR WAS SHOWN, carried back with the submit.
    //
    // These hidden fields are how "I only changed WiFi" becomes knowable. Without them the
    // server cannot tell a field the operator edited from one they merely left as it was
    // pre-filled, and storing the pre-filled value would freeze today's Factory default
    // into NVS as if it had been chosen — so a later secrets.h edit would be ignored.
    //
    // The passphrase is deliberately absent: it is never pre-filled, so there is nothing
    // to compare against and nothing to leak.
    p += hidden("o_ssid", v.ssid);
    p += hidden("o_host", v.host);
    p += hidden("o_ip", v.ip);
    p += hidden("o_weather", v.weather);

    p += "</form></body></html>";
    return p;
}

// ── validation ───────────────────────────────────────────────────────────────
// Strict dotted quad: exactly four decimal octets, 0-255, no leading '+', no spaces, no
// trailing dot. strtol would accept "0x10" and leading whitespace, both of which look
// like a typo to an operator rather than an intent.
static bool validIp(const char* s) {
    if (!s || !*s) return false;
    const char* p = s;
    for (int octet = 0; octet < 4; ++octet) {
        if (*p < '0' || *p > '9') return false;
        unsigned val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned)(*p - '0');
            if (++digits > 3 || val > 255) return false;
            ++p;
        }
        if (octet < 3) {
            if (*p != '.') return false;
            ++p;
        }
    }
    return *p == '\0';
}

bool provValidate(const prov_form_values_t& v, prov_form_error_t* err) {
    prov_form_error_t local = PROV_FORM_OK;

    // Every field is checked for LENGTH and shape only. A blank is always acceptable at
    // this stage: what a blank MEANS is decided by provApply, per field, because "leave my
    // Server alone" (a WiFi-only submit) and "clear the Server IP" are both blank inputs
    // with opposite intent. Rejecting blanks here would refuse every partial submit, which
    // is the opposite of what per-field fallback is for.
    //
    // The one thing that must never pass is a Server IP that is present but malformed:
    // silently ignoring it would leave the Board pointed at a stale address with no
    // complaint, which is the failure this ticket exists to prevent.
    if (len(v.ssid) > kMaxSsid)                local = PROV_FORM_ERR_SSID_TOO_LONG;
    else if (len(v.pass) > kMaxPass)           local = PROV_FORM_ERR_PASS_TOO_LONG;
    else if (len(v.host) > kMaxHost)           local = PROV_FORM_ERR_SERVER_HOST;
    else if (len(v.ip) > 0 && !validIp(v.ip))  local = PROV_FORM_ERR_SERVER_IP;
    else if (len(v.weather) > kMaxWeather)     local = PROV_FORM_ERR_WEATHER_TOO_LONG;

    if (err) *err = local;
    return local == PROV_FORM_OK;
}

bool provApply(const prov_form_values_t& sub,
               const prov_form_values_t& orig,
               prov_form_error_t* err) {
    if (!provValidate(sub, err)) return false;

    // A field is stored only when it DIFFERS from what the form was showing. Blankness
    // cannot decide this: the form pre-fills, so a WiFi-only submit arrives carrying the
    // Server's current value — which on a never-provisioned Board is the Factory default,
    // and storing that would pin it as a deliberate choice.
    const bool ssidChanged    = strcmp(nz(sub.ssid),    nz(orig.ssid))    != 0;
    const bool hostChanged    = strcmp(nz(sub.host),    nz(orig.host))    != 0;
    const bool ipChanged      = strcmp(nz(sub.ip),      nz(orig.ip))      != 0;
    const bool weatherChanged = strcmp(nz(sub.weather), nz(orig.weather)) != 0;

    // The passphrase cannot be compared — it is never pre-filled — so a non-blank one is
    // always a change. A blank one keeps the stored password: storing an empty passphrase
    // would leave the Board unable to join any secured network.
    const bool passGiven = len(sub.pass) > 0;

    // A CHANGE to the SSID or hostname cannot be a clearing: a Board with no SSID can
    // never connect, and a Board with no Server address has nothing to poll. Emptying
    // either is never a sensible instruction, so an emptied required field is ignored
    // rather than obeyed. Named rather than inlined because `a && b || c` is exactly the
    // precedence the firmware's -Werror refuses to guess at.
    const bool ssidGiven = ssidChanged && len(sub.ssid) > 0;
    const bool hostGiven = hostChanged && len(sub.host) > 0;

    if (ssidGiven) cfgSetWifiSsid(sub.ssid);
    if (passGiven) cfgSetWifiPass(sub.pass);
    if (ssidGiven || passGiven) cfgSaveWifi();

    if (hostGiven) cfgSetServerHost(sub.host);
    // The IP is the one field where blanking IS a real instruction: the AC requires that
    // blanking it mean "no static fallback", not a fallback to the Factory default, which
    // would silently point the Board at the wrong machine. An operator who clears a
    // pre-filled address gets CFG_IP_EMPTY, not the default.
    if (ipChanged) cfgSetServerIp(sub.ip);
    if (hostGiven || ipChanged) cfgSaveServer();

    if (weatherChanged) cfgSetWeatherLocation(sub.weather);
    if (weatherChanged) cfgSaveWeather();

    return true;
}
