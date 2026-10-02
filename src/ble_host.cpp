// ble_host.cpp — NimBLE central implementing HID-over-GATT for the Keychron
// bridge (see ble_host.h for the flow & rationale).
//
// IMPORTANT threading model: NimBLE callbacks (scan results, connect success,
// notify) run on the NimBLE host task. We never touch USB/display from those
// callbacks — decoded keyboard reports are pushed into a FreeRTOS queue and the
// app's BLE task (bleTick) drains it and re-emits over TinyUSB HID.
//
// Bonding: Just-Works (no PIN) is accepted with bonding enabled; the bond (LTK)
// is persisted by NimBLE-Arduino's default NVS bond store, so a bonded Keychron
// reconnects without re-pairing. bleTick() drives scan/connect/reconnect.
//
// API target: NimBLE-Arduino 1.4.3 (what PlatformIO resolves from ^1.4.0).

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <NimBLEDevice.h>

#include "ble_host.h"
#include "board.h"

// ── configuration ────────────────────────────────────────────────────────────
// How the board picks the keyboard. Mode "any" = first device advertising the
// Generic HID service (0x1812), whatever its name. Mode "prefix" = name must
// match kKbdNamePrefix AND advertise 0x1812 (name patterns with a '?' wildcard
// for one char, e.g. "K?"). If no named device is found, the fallback automatically
// accepts the first pure-HID device (boot-only keyboards) seen in the same
// scan pass, so a misconfigured pattern can't brick the pairing.
//
// 2026-10-02: Keychron K-series do not advertise "Keychron..." — the K2 was
// proven to advertise nothing at all, and K8-style names are model-only
// ("K8", "K8PRO", ...). Default to "any" (pure-HID service match + unnamed
// pure-HID fallback) unless the sniffer proves a specific name.
static ble_match_mode_t g_matchMode = BLE_MATCH_ANY;
static char g_prefixBuf[16] = "K";        // used only in BLE_MATCH_PREFIX mode
static const char* kKbdNamePrefix = g_prefixBuf;
static const int   kScanDurationS   = 5;    // per scan pass
static const int   kReconnectDelayMs = 3000; // between reconnect attempts
static const int   kScanRestartDelayMs = 5000;
static const int   kQueueLen = 16;
static const uint16_t kKbdServiceUuid   = 0x1812; // Generic HID
static const uint16_t kKbdNotifUuid     = 0x2A4D; // Report (input reports as notifications)
static const uint16_t kMapUuid          = 0x2A4B; // Report Map (read)
static const uint16_t kProtoUuid        = 0x2A4E; // Protocol Mode (write 0x00)

// ── module state ─────────────────────────────────────────────────────────────
static ble_report_cb_t g_onReport = nullptr;
static volatile ble_state_t g_state = BLE_FREE;
static volatile uint32_t g_lastEventAt = 0;   // millis of last state change
static TaskHandle_t g_bleTask = nullptr;
static QueueHandle_t g_reportQ = nullptr;     // decoded reports -> bridge
static NimBLEClient* g_client = nullptr;      // single client, created in init
static NimBLEAddress g_kbdAddr;               // last seen keyboard address
static bool g_hasAddr = false;

static const char* kStateNames[] = {
    "free", "scanning", "connecting", "connected", "paired", "lost", "error" };

const char* bleStateName(ble_state_t s) {
    int i = (int)s;
    return (i >= 0 && i < (int)(sizeof(kStateNames) / sizeof(kStateNames[0])))
        ? kStateNames[i] : "?";
}

ble_state_t bleLastEvent(void) { return g_state; }
bool bleIsConnected(void) { return g_state == BLE_CONNECTED || g_state == BLE_PAIRED; }

void bleSetState(ble_state_t s) {
    if (g_state != s) {
        g_state = s;
        g_lastEventAt = millis();
        Serial.printf("[ble] state -> %s\n", bleStateName(s));
    }
}

// Report queue payload: { heap data, len }. Filled in the notify callback
// (NimBLE host task), drained on the BLE task in bleTick().
struct HidReport {
    uint8_t* data;
    uint16_t len;
};

// ── scanner ──────────────────────────────────────────────────────────────────
// A device qualifies if it advertises the Generic HID service (0x1812); in
// BLE_MATCH_PREFIX mode it must ALSO carry the configured name prefix. A
// boot-only keyboard advertising only the HID service (no name at all) is
// caught by the pure-HID fallback below.
static bool isNamedKeyboard(const char* nm) {
    const char* pfx = kKbdNamePrefix;
    for (size_t i = 0; pfx[i]; i++) {
        if (pfx[i] == '?') continue;                    // wildcard = one char
        if (nm[i] != pfx[i]) return false;
    }
    return true;
}

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* adv) override {
        const char* nm = adv->getName().c_str();
        bool isHid = adv->isAdvertisingService(NimBLEUUID(kKbdServiceUuid));
        bool named = g_matchMode == BLE_MATCH_ANY || (isHid && nm && nm[0] && isNamedKeyboard(nm));
        bool pureHidFallback = isHid && !nm[0];   // no advertised name, HID only -> boot keyboard
        if (named || pureHidFallback) {
            Serial.printf("[ble] found keyboard %s (%s), rssi=%d%s\n",
                          adv->getAddress().toString().c_str(),
                          nm[0] ? nm : "(unnamed)", adv->getRSSI(),
                          pureHidFallback ? " [pure-HID]" : "");
            g_kbdAddr = adv->getAddress();
            g_hasAddr = true;
            NimBLEDevice::getScan()->stop();
            bleSetState(BLE_CONNECTING);   // bleTick() performs the connect
        } else {
            // Keep common keyboards visible even if they skip the name.
            Serial.printf("[ble] adv %s name='%s' hid=%d\n",
                          adv->getAddress().toString().c_str(),
                          nm, (int)isHid);
        }
    }
};

void bleSetMatchMode(ble_match_mode_t m, const char* prefix) {
    g_matchMode = m;
    if (prefix && prefix[0]) {
        strncpy(g_prefixBuf, prefix, sizeof(g_prefixBuf) - 1);
        g_prefixBuf[sizeof(g_prefixBuf) - 1] = 0;
    }
    // Scanner reads these on the NimBLE host task; a stale read is benign.
    Serial.printf("[ble] match mode=%s prefix='%s'\n",
                  m == BLE_MATCH_ANY ? "any" : "prefix",
                  kKbdNamePrefix);
}

// Non-blocking scan start (NimBLE 1.4: start(duration, cb) returns immediately).
void bleStartScan(void) {
    if (g_state != BLE_FREE && g_state != BLE_SCANNING) return;
    bleSetState(BLE_SCANNING);
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new ScanCB(), false);
    // Active scan (interval 45, window 15) reliably hears the environment's
    // BLE devices; use it for detection. Keep the values NimBLE-default-ish.
    pScan->setActiveScan(true);
    pScan->setInterval(45);
    pScan->setWindow(15);
    if (!pScan->isScanning()) {
        Serial.println("[ble] scan start");
        pScan->start(kScanDurationS);   // non-blocking (returns immediately)
    }
}

void bleStopScan(void) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan) pScan->stop();
}

// ── client / GATT ────────────────────────────────────────────────────────────
class ClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        bleSetState(BLE_CONNECTED);
        Serial.printf("[ble] connected to %s\n",
                      pClient->getPeerAddress().toString().c_str());
    }
    void onDisconnect(NimBLEClient* pClient) override {
        if (g_state == BLE_CONNECTED || g_state == BLE_PAIRED) {
            bleSetState(BLE_LOST);
            Serial.println("[ble] disconnected");
        }
    }
    uint32_t onPassKeyRequest(void) override { return 0; }        // Just Works: no PIN
    bool onConfirmPIN(uint32_t pin) override { return true; }
};

// Called by NimBLE on each 0x2A4D notification (host task context). We only
// copy + queue here; decoding/re-emitting happens on the BLE task.
static void kbdNotifyCB(NimBLERemoteCharacteristic* pChr, uint8_t* pData,
                        size_t len, bool isNotify) {
    if (!g_onReport || !pData || len == 0) return;
    uint8_t* copy = (uint8_t*)malloc(len);
    if (!copy) return;
    memcpy(copy, pData, len);
    HidReport r = { copy, (uint16_t)len };
    xQueueSend(g_reportQ, &r, 0);
}

// Decode + re-emit runs here, on the BLE task (never on the NimBLE host task,
// and never on the render core). Called from bleTick().
static void drainReports(void) {
    HidReport r;
    while (xQueueReceive(g_reportQ, &r, 0) == pdTRUE) {
        if (g_onReport && r.data && r.len) g_onReport(r.data, r.len);
        free(r.data);
    }
}

// Read the HID report map (0x2A4B). NimBLE 1.4's readValue() uses
// ble_gattc_read_long, which handles long maps automatically. The descriptor
// bytes themselves aren't needed to bridge boot-protocol reports — this read
// exists to satisfy the standard flow (and the MicroPython #17807 gotcha).
static bool readReportMap(NimBLERemoteService* svc, uint16_t* outLen) {
    NimBLERemoteCharacteristic* map = svc->getCharacteristic(NimBLEUUID(kMapUuid));
    if (!map) { Serial.println("[ble] no report map char"); return false; }
    NimBLEAttValue val = map->readValue();
    if (!val.size()) { Serial.println("[ble] report map read empty"); return false; }
    *outLen = val.size();
    Serial.printf("[ble] report map: %u bytes\n", (unsigned)val.size());
    return true;
}

// Connect + configure the keyboard GATT. Runs on the BLE task (NOT from a
// NimBLE host callback). Synchronous: blocks until connect or timeout.
static void connectKbd(const NimBLEAddress& addr) {
    if (g_state == BLE_CONNECTED || g_state == BLE_PAIRED) return;
    bleSetState(BLE_CONNECTING);
    if (!g_client) return;

    g_client->deleteServices();          // fresh discovery every (re)connect
    if (!g_client->connect(addr, true)) {
        Serial.println("[ble] connect failed");
        if (g_state != BLE_LOST) bleSetState(BLE_LOST);
        return;
    }
    NimBLERemoteService* svc = g_client->getService(NimBLEUUID(kKbdServiceUuid));
    if (!svc) {
        Serial.println("[ble] HID service 0x1812 not found");
        g_client->disconnect();
        bleSetState(BLE_LOST);
        return;
    }

    // 1) protocol mode -> report mode (0x00)
    NimBLERemoteCharacteristic* proto = svc->getCharacteristic(NimBLEUUID(kProtoUuid));
    if (proto && proto->canWrite()) {
        uint8_t zero = 0;
        proto->writeValue(&zero, 1, false);
    }

    // 2) report map (read only; sized for validation — not needed to bridge)
    uint16_t mapLen = 0;
    readReportMap(svc, &mapLen);

    // 3) subscribe to Report notifications via CCCD
    NimBLERemoteCharacteristic* rep = svc->getCharacteristic(NimBLEUUID(kKbdNotifUuid));
    if (!rep || !rep->canNotify()) {
        Serial.println("[ble] no notifiable Report characteristic");
        g_client->disconnect();
        bleSetState(BLE_LOST);
        return;
    }
    bool ok = rep->subscribe(true, kbdNotifyCB, false);
    if (!ok) {
        Serial.println("[ble] subscribe failed");
        g_client->disconnect();
        bleSetState(BLE_LOST);
        return;
    }
    bleSetState(BLE_PAIRED);
    Serial.println("[ble] paired + subscribed (typing bridge active)");
}

// Drives the state machine: called periodically from the BLE task.
void bleTick(void) {
    drainReports();
    switch (g_state) {
        case BLE_FREE:
            if (millis() - g_lastEventAt > kScanRestartDelayMs) bleStartScan();
            break;
        case BLE_SCANNING:
            // A scan pass has a finite duration (kScanDurationS). Once it ends
            // the scanner idles, so re-fire it to keep polling for the keyboard.
            // bleStartScan() guards on !isScanning(), so this is a no-op while a
            // pass is still in flight.
            if (!NimBLEDevice::getScan()->isScanning()) bleStartScan();
            break;
        case BLE_CONNECTING:
            if (g_hasAddr && millis() - g_lastEventAt > 1000)
                connectKbd(g_kbdAddr);
            break;
        case BLE_LOST:
            if (g_hasAddr && millis() - g_lastEventAt > kReconnectDelayMs) {
                Serial.println("[ble] reconnecting...");
                bleSetState(BLE_CONNECTING);
                connectKbd(g_kbdAddr);
            } else if (!g_hasAddr) {
                bleStartScan();
            }
            break;
        default:
            break;
    }
}

void bleDisconnect(void) {
    if (g_client && g_client->isConnected()) g_client->disconnect();
    if (g_state != BLE_FREE) bleSetState(BLE_FREE);
    Serial.println("[ble] disconnected by user");
}

// BLE task: drives the state machine + drains the report queue to the bridge.
static void bleTaskFn(void*) {
    for (;;) {
        bleTick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void bleInit(ble_report_cb_t onReport) {
    g_onReport = onReport;
    g_reportQ = xQueueCreate(kQueueLen, sizeof(HidReport));
    if (!g_reportQ) Serial.println("[ble] report queue failed");
    bleSetState(BLE_FREE);

    NimBLEDevice::init("llm-tick-kbd");       // host device name
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
    // Prefer a large ATT MTU so long report-map reads happen in one round trip
    // (NimBLE 1.4: must be set before the connection is established).
    NimBLEDevice::setMTU(517);
    // Just-Works pairing: bond enabled, no MITM/passkey. BLE-HID keyboards are
    // "Input Only" — Just Works is the only valid SM method.
    NimBLEDevice::setSecurityAuth(false, false, true);

    // One client for the whole session; connect(addr) sets the peer address.
    g_client = NimBLEDevice::createClient();
    if (g_client) g_client->setClientCallbacks(new ClientCB(), false);
    Serial.printf("[ble] NimBLE init done (client=%p)\n", (void*)g_client);

    BaseType_t rc = xTaskCreatePinnedToCore(bleTaskFn, "ble", 8192, nullptr, 2, &g_bleTask, 1);
    Serial.printf("[ble] task rc=%d\n", (int)rc);
}

// -- not used yet; placeholder for pairing UX (web/Page later) -----------------
void bleUnpairAll(void) {
    bleDisconnect();
    NimBLEDevice::deleteAllBonds();
    g_hasAddr = false;
    Serial.println("[ble] all bonds deleted");
}