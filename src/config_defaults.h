// config_defaults.h — maps the Factory defaults (secrets.h) onto the CFG_DEF_* names
// that the Config seam is built against.
//
// Kept separate from config.h so the seam's own header stays free of secrets.h: the
// host test compiles config.cpp with -DCFG_DEF_* and never sees this file, so the test
// needs no credentials at all.
//
// Included by config.cpp BEFORE config.h, because config.h's getters reference the
// CFG_DEF_* macros.
#pragma once

// Two-step stringify. Must be defined BEFORE secrets.h, which uses CFG_STR to build
// SERVER_IP_TEXT from its four octet macros. A one-level # would not expand the octet
// macros first. (Stringifying the COMMA-separated SERVER_IP_OCTETS directly is not
// possible: the preprocessor reads its commas as argument separators, so the octets are
// kept separate in secrets.h and joined here.)
#define CFG_STR_(x) #x
#define CFG_STR(x)  CFG_STR_(x)

#include "secrets.h"

// MIGRATION GUARD. An older checkout's gitignored secrets.h declares
// `SERVER_IP_OCTETS 192, 168, 1, 100` as a single comma-separated macro and has no
// SERVER_IP_A..D. Without this guard the failure is an undefined identifier deep in a
// template, which says nothing useful. The comma form cannot be stringified (the
// preprocessor reads its commas as argument separators), which is why secrets.h now
// carries four separate octet macros.
#ifndef SERVER_IP_TEXT
#error "secrets.h is out of date: it has no SERVER_IP_TEXT. Re-copy secrets.h.example \
to secrets.h and re-enter your values (the Server IP is now four SERVER_IP_A..D octets)."
#endif

#define CFG_DEF_WIFI_SSID        WIFI_SSID
#define CFG_DEF_WIFI_PASS        WIFI_PASS
#define CFG_DEF_SERVER_HOST      SERVER_HOST
#define CFG_DEF_SERVER_IP        SERVER_IP_TEXT
#define CFG_DEF_SERVER_PORT      SERVER_PORT
#define CFG_DEF_WEATHER_LOCATION WEATHER_LOCATION
