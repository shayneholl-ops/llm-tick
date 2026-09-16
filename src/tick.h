// tick.h — shared state for the llm-tick firmware (single sketch, split across
// a few files for readability; Arduino compiles them together).
#pragma once
#include <Arduino.h>
#include "board.h"

// ── Usage data (populated on core 1 by fetchUsage, drawn on core 0) ─────────
// Benign: every field is 32-bit-or-smaller, so tearing is not possible.
struct ModelUsage { char name[12]; int pct; char reset[10]; };
struct TokModel   { char name[12]; int pct; char tok[8]; float cost; };

struct Usage {
  bool ok = false;            // last parse had real data
  bool stale = false;        // serving last-good after an upstream failure
  bool ccOk = false;         // a local token source answered
  int sessionPct = -1, weeklyPct = -1, sessionResetMin = 0;
  char weeklyReset[10] = "";
  char status[10] = "wait";
  ModelUsage models[4]; int modelCount = 0;
  TokModel tokModels[4]; int tokModelCount = 0;
  char tokActive[8] = "", tokWeek[8] = "";
  float burnHr = 0;
  int  spendPct = -1; float spendUsed = 0, spendLimit = 0;
  char spendCur[4] = "";
  unsigned long fetchedAgo = 0;     // s since last successful fetch
  unsigned long dataChangedMs = 0;  // last time the numbers actually moved
  unsigned long lastFetchMs = 0;
  int  curPage = 0;
};
extern Usage g_u;

extern volatile float g_ax, g_ay, g_az;
extern volatile uint32_t g_lastSceneChange;   // set by checkButton / auto-switch

// ── Provided by llm-tick.ino ─────────────────────────────────────────────────
extern LGFX lcd;
extern LGFX_Sprite* sprites[2];
extern uint16_t*    bufs[2];
extern QueueHandle_t freeQ, readyQ;
extern volatile int  g_scene;
int    sceneCount();
const char* sceneName(int s);
void showLed(int s);

// ── ui.cpp ───────────────────────────────────────────────────────────────────
void renderUiScene(int scene, uint16_t* buf, int w, int h);

// ── data.cpp ────────────────────────────────────────────────────────────────
void  wifiInit();
void  ntpWait();
bool  resolveServer();
void  fetchUsage();
void  refreshWeather();
void  tickLogic();
int   usagePageCountOf(const Usage& u);

// ── Weather ──────────────────────────────────────────────────────────────────
#include "weather_api.h"
extern WeatherAPI g_wx;
extern WeatherData g_wxData;
