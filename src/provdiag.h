// provdiag.h — the PROV_NO_HTTP differential diagnostic. See the bottom of this file.
//
// There is NOTHING TO DECLARE here, deliberately: the switch is a compile-time
// -DPROV_NO_HTTP=1 that prov.cpp branches on directly, so this file carries the
// reasoning and no API. It exists as a header only so the note has a name that search
// and review will actually surface — the earlier version of it opened with the
// comment "provdiag.cpp —", which is a different file, and was included by nothing.
//
// WHAT THIS DIAGNOSTIC SETTLED (2026-10-02, issue #6)
// ------------------------------------------------------
// The Setup AP crashed ~1 s after a client associated. Two candidates: the WebServer
// (handleClient / request parsing / response writing), or the WiFi driver's receive
// path.
//
// Build with the server compiled out and the crash still happened, which exonerated
// the server and pointed at the radio. Decoding the new backtrace then gave
//   wDev_ProcessRxSucData -> esf_buf_alloc_dynamic -> wifi_malloc -> tlsf_malloc
// and the real cause: the driver's DYNAMIC RX buffer pool corrupting TLSF metadata.
// Fixed structurally in sdkconfig.defaults (DYNAMIC_RX_BUFFER_NUM=0), which only works
// for env:esp32s3-idf — the plain-Arduino env links a prebuilt driver and ignores it.
//
// The env is kept (see platformio.ini, [env:esp32s3-nohttp]) so this conclusion stays
// re-checkable against a future driver change rather than becoming folklore.
#pragma once