// data.cpp — connectivity + the data brain: mDNS discovery, JSON poll of
// server.py, idle->standby switching, weather refresh, wifi backstop.
#include "tick.h"
#include "secrets.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

Usage g_u;
volatile uint32_t g_lastSceneChange = 0;

WeatherAPI g_wx;
WeatherData g_wxData;

static IPAddress g_serverIp;
static String   g_serverUrl;
static unsigned long g_reconnectBackoff = 15000;
static unsigned long g_lastReconnect = 0;
static const IPAddress kFallbackIp(SERVER_IP_OCTETS);

// ponytail: 60s poll is a hard cap; server.py caches 180s server-side anyway.
const unsigned long FETCH_INTERVAL = 60000;
const unsigned long IDLE_TO_WEATHER = 90000;

void wifiInit() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setMinSecurity(WIFI_AUTH_WEP);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_13dBm);   // S3s fail auth at full TX power
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  // NOTE: do NOT draw through the panel `lcd` here — a text op on the panel
  // triggers flush(), which sends the panel's own (empty) buffer to the screen
  // and overwrites the pushed frame with black. The pipeline owns the panel.
  while (WiFi.status() != WL_CONNECTED && tries < 40) { delay(500); tries++; }
  Serial.printf("[tick] WiFi %s (tries=%d)\n",
                WiFi.status() == WL_CONNECTED ? "UP" : "FAIL", tries);
  // Re-assert backlight (defensive; it's set in setup, and nothing else touches it)
  digitalWrite(PIN_BL, HIGH);
  // GPIO46 is NOT the backlight on this unit (blink-matrix confirmed 48) — hold it
  // LOW so it can't fight the BL net.
  pinMode(46, OUTPUT);
  digitalWrite(46, LOW);
}

void ntpWait() {
  configTzTime("PST8PDT", "pool.ntp.org");
  for (int i = 0; i < 20 && time(nullptr) < 2000000000; i++) delay(1000);
}

bool resolveServer() {
  IPAddress found = MDNS.queryHost(SERVER_HOST, 3000);
  g_serverIp = (uint32_t)found != 0 ? found : kFallbackIp;
  g_serverUrl = "http://" + g_serverIp.toString() + ":" + String(SERVER_PORT) + "/usage";
  Serial.printf("[mDNS] %s.local -> %s%s\n", SERVER_HOST, g_serverIp.toString().c_str(),
                (uint32_t)found != 0 ? "" : "  (no answer, using fallback)");
  return (uint32_t)found != 0;
}

static void copyStr(char* dst, size_t n, const String& s) {
  size_t m = min((size_t)n - 1, s.length());
  memcpy(dst, s.c_str(), m);
  dst[m] = 0;
}

// Pct fields may arrive as int OR float (server sends round(x,1)); `v | -1`
// silently falls back to the default for floats (is<int>() is false), so read
// them explicitly.
static int pctOr(JsonDocument& doc, const char* key, int fallback = -1) {
  JsonVariant v = doc[key];
  if (v.is<int>() || v.is<float>()) return v.as<int>();
  return fallback;
}

static void parseUsage(const String& payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  // session_pct may arrive as a JSON float (server sends round(x,1)); accept
  // int OR float, else the whole payload is rejected and the UI stays "stale".
  if (err || !(doc["session_pct"].is<int>() || doc["session_pct"].is<float>())) { g_u.stale = true; return; }

  Usage u = g_u;   // keep curPage / lastFetchMs
  u.ok = true; u.stale = false;
  u.sessionPct      = pctOr(doc, "session_pct");
  u.weeklyPct      = pctOr(doc, "weekly_pct");
  u.sessionResetMin = doc["session_reset_min"] | 0;
  copyStr(u.weeklyReset, sizeof(u.weeklyReset), doc["weekly_reset_day"].as<String>());
  copyStr(u.status, sizeof(u.status), doc["status"].as<String>());
  u.ccOk = doc["cc_ok"] | false;
  copyStr(u.tokActive, sizeof(u.tokActive), doc["tok_active"].as<String>());
  copyStr(u.tokWeek,   sizeof(u.tokWeek),   doc["tok_week"].as<String>());
  u.burnHr = doc["burn_hr"] | 0.0f;
  u.modelCount = 0;
  for (JsonObject m : doc["models"].as<JsonArray>()) {
    if (u.modelCount >= 4) break;
    copyStr(u.models[u.modelCount].name, 12, m["name"].as<String>());
    u.models[u.modelCount].pct = m["pct"] | 0;
    copyStr(u.models[u.modelCount].reset, 10, m["reset_day"].as<String>());
    u.modelCount++;
  }
  u.tokModelCount = 0;
  for (JsonObject t : doc["tok_models"].as<JsonArray>()) {
    if (u.tokModelCount >= 4) break;
    copyStr(u.tokModels[u.tokModelCount].name, 12, t["name"].as<String>());
    u.tokModels[u.tokModelCount].pct = t["pct"] | 0;
    copyStr(u.tokModels[u.tokModelCount].tok, 8, t["tok"].as<String>());
    u.tokModels[u.tokModelCount].cost = t["cost"] | 0.0f;
    u.tokModelCount++;
  }
  if (doc["spend_pct"].is<int>()) {
    u.spendPct = doc["spend_pct"] | -1;
    u.spendUsed = doc["spend_used"] | 0.0f;
    u.spendLimit = doc["spend_limit"] | 0.0f;
    copyStr(u.spendCur, 4, doc["spend_cur"].as<String>());
  }
  // GPU telemetry (server.py -> SSH -> rocm-smi). Absent or failed = gpuOk false.
  u.gpuOk = doc["gpu_ok"] | false;
  u.gpuLoadPct = doc["gpu_load_pct"] | -1;
  u.gpuTempC = doc["gpu_temp_c"] | -1.0f;
  u.gpuTempJunctionC = doc["gpu_temp_junction_c"] | -1.0f;

  // Did anything actually move? That's the real "is the LLM busy" signal.
  bool changed = (u.sessionPct != g_u.sessionPct) || (u.weeklyPct != g_u.weeklyPct)
             || memcmp(u.tokActive, g_u.tokActive, 8)
             || memcmp(u.tokWeek, g_u.tokWeek, 8);
  u.lastFetchMs = millis();
  u.fetchedAgo = 0;
  if (changed) u.dataChangedMs = millis();
  if (u.curPage >= usagePageCountOf(u)) u.curPage = 0;
  g_u = u;
  Serial.printf("[tick] usage ok: session=%d%% weekly=%d%% models=%d tok=%s\n",
                u.sessionPct, u.weeklyPct, u.modelCount, u.tokActive);
}

void fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.begin(g_serverUrl);
  int code = http.GET();
  if (code < 0) {  // weak-signal SYN loss: one quick retry
    http.end(); delay(600);
    http.begin(g_serverUrl);
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    code = http.GET();
  }
  if (code == 200) {
    String payload = http.getString();
    if (payload.length() < 64000) parseUsage(payload);
    else g_u.stale = true;
  } else {
    g_u.stale = true;
    Serial.printf("[tick] fetch HTTP %d\n", code);
    if (code < 0) resolveServer();   // the server may have moved
  }
  http.end();
  g_u.lastFetchMs = millis();
  g_u.fetchedAgo = 0;
}

void refreshWeather() {
  bool ok = g_wx.fetch();
  if (ok) g_wxData = g_wx.get();
  Serial.printf("[tick] wx fetch %s (temp %.0fC)\n", ok ? "ok" : "FAIL", ok ? g_wxData.temperature : 0.0f);
}

int usagePageCountOf(const Usage& u) {
  const int BPP = 3;
  int n = 1;
  if (u.ccOk && u.tokModelCount > 0) n += 1;
  if (u.modelCount > 0) n += (u.modelCount + BPP - 1) / BPP;
  return n;
}

void tickLogic() {
  unsigned long now = millis();
  g_u.fetchedAgo = (now - g_u.lastFetchMs) / 1000;

  // Poll cadence: 60s normally, 10s after a failure. A scene switch refreshes
  // promptly, but at most once per 30 s: hammering BOOT in a tight loop must
  // not burst the wifi RX path (2026-09-19: ~10 fetches/min crash-looped the
  // driver's ebuf pool on EVERY build, the pristine 2026-09-18 one included).
  // The usage page polls faster (15s) because its GPU row is a *live* gauge —
  // a 60s-old load figure is worthless. Gated to scene 0, page 0 (where that
  // row is drawn) so no extra wifi traffic while idle on weather or on the
  // slower pages, and still well under the 10/min that hurt the RX pool.
  const unsigned long LIVE_INTERVAL = 15000;
  unsigned long interval = (g_u.ok && !g_u.stale) ? FETCH_INTERVAL : 10000;
  if (g_scene == 0 && g_u.curPage == 0 && g_u.ok && !g_u.stale
      && interval > LIVE_INTERVAL)
    interval = LIVE_INTERVAL;
  if (now - g_u.lastFetchMs > interval ||
      ((now - g_lastSceneChange) < 500 && (now - g_u.lastFetchMs) > 30000))
    fetchUsage();

  // IDLE -> standby: usage data unchanged for 90s and not just switched back.
  if (g_scene == 0 && g_u.ok
      && (now - g_u.dataChangedMs) > IDLE_TO_WEATHER
      && (now - g_lastSceneChange) > IDLE_TO_WEATHER) {
    g_scene = 1;
    g_lastSceneChange = now;
    showLed(1);
    refreshWeather();
    Serial.println("[tick] idle -> weather standby");
  }
  // ACTIVE -> back to usage: data moved while parked on the standby screen.
  else if (g_scene == 1 && g_u.ok && (now - g_u.dataChangedMs) < 30000) {
    g_scene = 0;
    g_u.curPage = 0;
    g_lastSceneChange = now;
    showLed(0);
    Serial.println("[tick] activity -> usage");
  }

  // Weather: same 30 s anti-burst gate on the on-switch refresh (HTTPS is the
  // heaviest RX load the driver gets); the 30-min needsUpdate cadence stays.
  static unsigned long lastWxRefresh = 0;
  if (g_scene == 1 &&
      (((now - g_lastSceneChange) < 500 && now - lastWxRefresh > 30000) || g_wx.needsUpdate())) {
    refreshWeather();
    lastWxRefresh = now;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (now - g_lastReconnect > g_reconnectBackoff) {
      WiFi.reconnect();
      g_lastReconnect = now;
      g_reconnectBackoff = min(g_reconnectBackoff * 2, 120000UL);
    }
  } else {
    g_reconnectBackoff = 15000;
  }
}
