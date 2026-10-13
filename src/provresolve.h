// provresolve.h — ticket #9's pure decision logic: what does the weather
// service's search answer mean?
//
// The HTTP call itself stays in the board wiring (it touches the radio; this does
// not). What lives here is the part that must be right no matter what the
// network does: telling "no place by that name" apart from "the service could
// not answer" (the ticket's two failures that must not collapse into one
// message), picking the canonical name from the answer, and never touching
// memory past the bytes actually received — a truncated body is an everyday
// event on this hardware's radio, not a corner case.
//
// Host-tested in tools/provresolvetest; the board compiles the same file.

#ifndef PROVRESOLVE_H
#define PROVRESOLVE_H

#include <ArduinoJson.h>
#include <cstddef>

enum prov_resolve_outcome_t {
    PROV_RESOLVE_OK = 0,   // a place exists; its canonical name was written
    PROV_RESOLVE_NO_MATCH, // the service answered; no place has that name
    PROV_RESOLVE_ERROR     // the body is unusable (not a results array) — the
                           // caller reports this as "the service could not be
                           // reached", deliberately NOT as "no such place"
};

// Parse a weatherapi search.json body (top-level array; an object carrying a
// "results" array is also accepted, defensively) into a decision.
//
// OK:      the first match's `name` is written to nameOut (NUL-terminated,
//          truncated to fit nameCap), and "Name, Region, Country" — with
//          whatever of the three is present — to whereOut, for the page echo.
//          The operator must be able to see what was understood BEFORE it
//          takes effect, so the echo string is part of the decision.
// NO_MATCH: an empty results array.
// ERROR:   nothing else (bad JSON, truncated, empty, a bare object without
//          "results"). Nothing is written.
//
// The function reads at most len bytes; it never assumes the body is
// NUL-terminated.
prov_resolve_outcome_t provResolveParse(const char* body, size_t len,
                                       char* nameOut, size_t nameCap,
                                       char* whereOut, size_t whereCap);

#endif // PROVRESOLVE_H
