// Percent-encode a form value for a URL query string (the value may contain
// spaces, apostrophes, commas, …). Everything except [A-Za-z0-9-_.~] becomes %XX.
//
// Shared by the two weatherapi.com callers so they treat the same stored
// location the same way: the provisioning resolve (prov.cpp) and the tick
// weather fetch (weather_api.cpp) — a resolved canonical name can contain a
// space, and a raw space in the request target is not a legal URL.
#pragma once
#include <Arduino.h>

static inline void urlEncode(const char* src, String& out) {
    static const char kHex[] = "0123456789ABCDEF";
    for (const char* p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += kHex[c >> 4]; out += kHex[c & 15]; }
    }
}
