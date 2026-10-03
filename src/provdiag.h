// provdiag.cpp — isolate WHAT crashes when a client associates with the Setup AP.
//
// The crash is a StoreProhibited on core 0, exactly when a station joins
// (`stations -> 1` immediately precedes the panic). Candidates, in order of
// likelihood:
//   1. the WebServer (handleClient / request parsing / response writing)
//   2. something else in the provisioning path that only runs on association
//
// A build flag lets the two be separated without editing code between flashes:
//   -DPROV_NO_HTTP=1   -> bring the AP up but DO NOT start the web server.
// If the crash survives with no server, it is not the server.
#pragma once
