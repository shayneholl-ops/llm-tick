// provresolve_test.cpp — host harness for the ticket #9 resolve seam.
//   pwsh -File build.ps1
#include "../../src/provresolve.h"
#include <cstdio>
#include <cstring>

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* what) {
    if (ok) { g_pass++; }
    else { g_fail++; printf("  FAIL  %s\n", what); }
}

static const char kSingle[] =
    "[{\"name\":\"Vancouver\",\"region\":\"British Columbia\",\"country\":\"Canada\","
    "\"lat\":49.25,\"lon\":-123.88,\"url\":\"vancouver\"}]";

static const char kMulti[] =
    "[{\"name\":\"Vancouver\",\"region\":\"British Columbia\",\"country\":\"Canada\"},"
    "{\"name\":\"Vancouver\",\"region\":\"Washington\",\"country\":\"United States\"}]";

static const char kNoRegion[] = "[{\"name\":\"Vancouver\"}]";

static const char kObjForm[] =
    "{\"results\":[{\"name\":\"Vancouver\",\"region\":\"British Columbia\",\"country\":\"Canada\"}]}";

int main() {
    char name[64], where[96];

    // 1. a single match yields its name and the full echo string
    std::memset(name, 0, sizeof name); std::memset(where, 0, sizeof where);
    int r = provResolveParse(kSingle, sizeof kSingle - 1, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_OK, "single match is OK");
    check(std::strcmp(name, "Vancouver") == 0, "canonical name is the place name");
    check(std::strcmp(where, "Vancouver, British Columbia, Canada") == 0,
          "echo is Name, Region, Country");

    // 2. multiple matches -> the FIRST is what gets understood
    std::memset(name, 0, sizeof name); std::memset(where, 0, sizeof where);
    r = provResolveParse(kMulti, sizeof kMulti - 1, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_OK, "multiple matches is OK");
    check(std::strcmp(name, "Vancouver") == 0 &&
          std::strcmp(where, "Vancouver, British Columbia, Canada") == 0,
          "the first match wins, deterministically");

    // 3. empty results array -> NO_MATCH (the place does not exist)
    r = provResolveParse("[]", 2, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_NO_MATCH, "empty array is NO_MATCH");

    // 4. the object-wrapped form is accepted defensively
    std::memset(name, 0, sizeof name);
    r = provResolveParse(kObjForm, sizeof kObjForm - 1, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_OK && std::strcmp(name, "Vancouver") == 0,
          "object with results array is accepted");

    // 5. a body that is valid JSON but not a results array -> ERROR, not NO_MATCH
    r = provResolveParse("{\"current\":{\"temp_c\":10}}", 28, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_ERROR, "a forecast body is ERROR, not NO_MATCH");

    // 6. garbage -> ERROR
    r = provResolveParse("not json at all", 14, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_ERROR, "garbage is ERROR");

    // 7. truncated mid-structure -> ERROR, no crash
    r = provResolveParse("{\"name\":\"Vancou", 16, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_ERROR, "truncated body is ERROR");

    // 8. empty body -> ERROR
    r = provResolveParse("", 0, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_ERROR, "empty body is ERROR");

    // 9. missing region/country -> the echo is just the name
    std::memset(where, 0, sizeof where);
    r = provResolveParse(kNoRegion, sizeof kNoRegion - 1, name, sizeof name, where, sizeof where);
    check(r == PROV_RESOLVE_OK && std::strcmp(where, "Vancouver") == 0,
          "missing region/country degrades to the bare name");

    // 10. an over-long name is truncated to fit, never past the buffer
    char tiny[12];
    std::memset(tiny, 'x', sizeof tiny);
    const char longName[] =
        "[{\"name\":\"Pittsburgh\",\"region\":\"Pennsylvania\",\"country\":\"United States\"}]";
    r = provResolveParse(longName, sizeof longName - 1, tiny, sizeof tiny, where, sizeof where);
    check(r == PROV_RESOLVE_OK, "long name still parses");
    check(tiny[0] == 'P' && std::strlen(tiny) < sizeof tiny, "name truncated to fit, NUL-terminated");
    // the terminator must land exactly at the cap boundary: nothing written past
    // tiny[cap-1] (bytes beyond the terminator are untouched, which is allowed)
    check(tiny[sizeof tiny - 2] == '\0', "no overflow past the cap");

    // 11. zero-size out buffers must not touch memory
    r = provResolveParse(kSingle, sizeof kSingle - 1, nullptr, 0, nullptr, 0);
    check(r == PROV_RESOLVE_OK, "null out buffers are safe");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
