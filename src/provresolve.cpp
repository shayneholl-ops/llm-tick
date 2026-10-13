// provresolve.cpp — ticket #9: decide what a weatherapi search answer means.
// Pure: no WiFi, no HTTP, no board — host- and board-compilable (TDD).
#include "provresolve.h"

#include <cstdio>
#include <cstring>

namespace {

// Copy src into dst (capacity cap), always NUL-terminating, never touching
// beyond dst[cap-1]. A zero cap means "no buffer"; nothing is written.
void fitCopy(const char* src, size_t srcLen, char* dst, size_t cap) {
    if (!dst || cap == 0) return;
    if (srcLen >= cap) srcLen = cap - 1;
    std::memcpy(dst, src, srcLen);
    dst[srcLen] = '\0';
}

// Append one piece of the echo ("Region", "Country") to a buffer we already
// started, keeping it NUL-terminated and inside cap.
void fitAppend(char* dst, size_t cap, const char* piece, size_t pieceLen) {
    if (!dst || cap == 0) return;
    size_t used = std::strlen(dst);
    if (used + 2 + pieceLen + 1 > cap) return;  // would not fit: drop the piece
    std::memcpy(dst + used, ", ", 2);
    std::memcpy(dst + used + 2, piece, pieceLen);
    dst[used + 2 + pieceLen] = '\0';
}

} // namespace

prov_resolve_outcome_t provResolveParse(const char* body, size_t len,
                                       char* nameOut, size_t nameCap,
                                       char* whereOut, size_t whereCap) {
    if (nameOut && nameCap) nameOut[0] = '\0';
    if (whereOut && whereCap) whereOut[0] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, body, len)) return PROV_RESOLVE_ERROR;

    // search.json is a top-level array. An object carrying a "results" array is
    // accepted defensively; anything else (a forecast body, an error object) is
    // unusable and must NOT be reported as "no such place".
    JsonArray arr;
    if (doc.is<JsonArray>()) {
        arr = doc.as<JsonArray>();
    } else if (doc.is<JsonObject>() && doc["results"].is<JsonArray>()) {
        arr = doc["results"];
    }
    if (arr == nullptr) return PROV_RESOLVE_ERROR;
    if (arr.size() == 0) return PROV_RESOLVE_NO_MATCH;

    const JsonObject first = arr[0].as<JsonObject>();
    if (first == nullptr) return PROV_RESOLVE_ERROR;

    const char* name = first["name"];
    if (!name || !*name) return PROV_RESOLVE_ERROR;

    const size_t nameLen = std::strlen(name);
    fitCopy(name, nameLen, nameOut, nameCap);

    // Echo "Name, Region, Country", degrading to whatever is present.
    fitCopy(name, nameLen, whereOut, whereCap);
    if (const char* region = first["region"]; region)
        fitAppend(whereOut, whereCap, region, std::strlen(region));
    if (const char* country = first["country"]; country)
        fitAppend(whereOut, whereCap, country, std::strlen(country));

    return PROV_RESOLVE_OK;
}
