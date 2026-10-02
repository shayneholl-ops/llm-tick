// ble_host.h — BLE central (NimBLE) HID-over-GATT client for the Keychron keyboard
// bridge. The S3 is the BLE *host*: it connects to the keyboard (peripheral),
// subscribes to the Boot-Report characteristic (0x2A4D) and forwards decoded
// key reports to the USBHIDKeyboard bridge.
//
// Implements the flow from .scratch/research/feature1-ble-keyboard-usb-bridge.md:
//   connect 0x1812 -> read Report Map 0x2A4B (chunked) -> write Protocol Mode
//   0x2A4E = 0x00 -> subscribe 0x2A4D via CCCD 0x2902 -> decode 64-byte report
//   (byte0 modifiers, byte1 reserved, byte2..7 keycodes).
//
// Exposes a tiny state machine to the app:
//   bleInit()            one-time init (NimBLE host + bond store in NVS)
//   bleStartScan()       scan for the keyboard by name (or any HID keyboard)
//   bleStopScan()
//   bleIsConnected()
//   bleDisconnect()
//   bleTick()            call from loop() (or a BLE task) to drive reconnects
//   bleLastEvent()       for WS2812 status indication
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_FREE = 0,        // nothing to do; idle
    BLE_SCANNING,        // scanning for the keyboard
    BLE_CONNECTING,      // found it, connecting
    BLE_CONNECTED,       // connected (notifications may or may not be armed yet)
    BLE_PAIRED,          // connected + 0x2A4D subscribed (+ report map read)
    BLE_LOST,            // was connected, link dropped (auto-reconnect armed)
    BLE_ERROR,           // unrecoverable config error
} ble_state_t;

// Called by the bridge on every decoded key report (a BLE HID input report or
// an LED/consumer report). Implemented in main.cpp (the USB re-emitter).
// `report` points to the raw 0x2A4D notification payload; len is its size.
typedef void (*ble_report_cb_t)(const uint8_t* report, uint16_t len);

// How the scanner picks the keyboard: "any" accepts the first device
// advertising the Generic HID service (0x1812), whatever its name; "prefix"
// requires the name to start with the configured prefix too. "any" is the
// default — Keychron K-series advertise model-only names ("K8", "K2", ...)
// or not at all.
typedef enum { BLE_MATCH_ANY = 0, BLE_MATCH_PREFIX } ble_match_mode_t;

void bleInit(ble_report_cb_t onReport);
void bleStartScan(void);
void bleStopScan(void);
bool bleIsConnected(void);
void bleDisconnect(void);
void bleTick(void);
ble_state_t bleLastEvent(void);
const char* bleStateName(ble_state_t s);
void bleSetMatchMode(ble_match_mode_t m, const char* prefix);  // "" = any name

#ifdef __cplusplus
}
#endif