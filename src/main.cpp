#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "board.h"
#include "cyberpunk.h"
#include "tick.h"

LGFX lcd;
LGFX_Sprite spr0(&lcd), spr1(&lcd);
LGFX_Sprite* sprites[2] = { &spr0, &spr1 };
LGFX_Sprite* uiSpr = &spr0;   // UI scene draws through this (into the active buffer)
uint16_t*    bufs[2]    = { nullptr, nullptr };
Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// Scenes: 0 = usage, 1 = weather standby, 2 = cyberpunk ambient (5 procedural
// sub-scenes that auto-cycle inside it — see cyberpunk.cpp). The serial PRESS
// line cycles them with one counter.
volatile int   g_scene = 0;
int            sceneCount() { return 3; }
const char*    sceneName(int s) { return s == 0 ? "usage" : (s == 1 ? "weather" : "cyberpunk"); }

volatile uint32_t g_renderUs = 0;

QueueHandle_t freeQ, readyQ;   // carry buffer indices (0/1) between the two cores

// ── DIAGNOSTIC (top-band flicker hunt, 2026-09-19) ────────────────────────────
// Extra serial commands (sent as one line each; see checkSerialCmd):
//   LED 0|1      WS2812 ring off / restore scene color
//   BL  0|25|50|100   backlight duty: off / 25% / 50% / full (1.5 kHz software PWM)
//   BLX 0..6   backlight channel probe — pins [48,46,47]: 0=normal (48 on),
//              1..3 = 48 on + 46-low / 47-low / both-low, 4..5 = 48 off + one low,
//              6 = all low. (2026-09-19: hunting a hidden white channel — the
//              2026-09-17 matrix only proved 46/47-HIGH is dead on this unit.)
//   PAT 0..6     0=normal scenes; 1=mid gray; 2=white top 24 rows / black rest;
//                3=black top 24 / gray rest; 4=all black; 5=all white;
//                6=black top 24 + black rows 150-173 / gray rest (position control)
//   ST           print diagnostic state
volatile int g_diagLed = 1;
volatile int g_diagBlDuty = 100;   // 0 = off, 100 = full
volatile int g_diagPat = 0;
volatile int g_wbField = 0;        // 0 = normal; 1..6 = raw WB calibration fields,
                                  // 7 = split AWB anchor (white | corrected dark canvas)

// WB calibration fields (logical 565; byte-swapped at write time, like the
// effect palettes): white, 50% gray, 25% gray, R, G, B primaries.
static const uint16_t wbFields[7] = { 0, 0xFFFF, 0x7BEF, 0x39E7, 0xF800, 0x07E0, 0x001F };

static const uint16_t kPatGray = 0x7BEF;
static void fillPattern(uint16_t* buf, int w, int h, int pat) {
  uint16_t topC = kPatGray, restC = kPatGray, midC = kPatGray;
  switch (pat) {
    case 1:  topC = restC = kPatGray; break;
    case 2:  topC = 0xFFFF; restC = 0x0000; break;
    case 3:  topC = 0x0000; restC = kPatGray; break;
    case 4:  topC = restC = 0x0000; break;
    case 5:  topC = restC = 0xFFFF; break;
    case 6:  topC = 0x0000; midC = 0x0000; restC = kPatGray; break;
  }
  for (int y = 0; y < h; y++) {
    uint16_t c = restC;
    if (y < 24) c = topC;
    else if (pat == 6 && y >= 150 && y < 174) c = midC;
    uint16_t* row = buf + (size_t)y * w;
    for (int x = 0; x < w; x++) row[x] = c;
  }
}

// Backlight driver: 200 Hz, 5-substep PWM via vTaskDelayUntil (1 ms ticks).
// NO delayMicroseconds — it busy-spins and a 100%-duty spin starves core 1
// (that froze the board on first try). 200 Hz aliases to DC in 4/25 fps
// camera captures (200/4=50, 200/25=8 exact) and is above flicker fusion.
// Duty 100 = pin HIGH every step (== plain HIGH); duty 0 = LOW every step.
static void blTask(void*) {
  TickType_t last = xTaskGetTickCount();
  int step = 0;
  for (;;) {
    int onSteps = (g_diagBlDuty * 5) / 100;
    digitalWrite(PIN_BL, step < onSteps ? HIGH : LOW);
    step = (step + 1) % 5;
    vTaskDelayUntil(&last, 1);   // 1 ms step; 5 steps = 5 ms period = 200 Hz
  }
}

void showLed(int s) {
  uint32_t c;
  if (s == 0)      c = led.Color(0, 24, 24);   // usage: blue
  else if (s == 1) c = led.Color(0, 20, 10);   // standby: soft green
  else             c = led.Color(0, 22, 16);   // cyberpunk: cyan
  led.setPixelColor(0, c);
  led.show();
}

// Producer (core 0): take a free buffer, render the active scene into it, hand
// it to the consumer. Effects write byte-swapped RGB565 directly; UI scenes draw
// through lcd into the same buffer (fonts are panel-bound on this core).
void renderTask(void*) {
  for (;;) {
    int idx;
    xQueueReceive(freeQ, &idx, portMAX_DELAY);
    int s = g_scene;
    uint32_t t = micros();
    uiSpr = sprites[idx];   // UI scene draws into this frame's buffer
    if (g_diagPat != 0) {
      fillPattern(bufs[idx], SCREEN_W, SCREEN_H, g_diagPat);
    } else if (g_wbField > 0) {
      if (g_wbField == 7) {
        // AWB anchor test: one half raw white (locks the camera's AWB at full
        // correction), the other half the corrected dark canvas the user
        // actually sees. Measured hue of the dark half = true residual cast.
        uint16_t dk = (uint16_t)((wb565(0x18C3) >> 8) | (wb565(0x18C3) << 8));
        for (int y = 0; y < SCREEN_H; y++) {
          uint16_t* row = bufs[idx] + (size_t)y * SCREEN_W;
          for (int x = 0; x < SCREEN_W / 2; x++) row[x] = 0xFFFF;
          for (int x = SCREEN_W / 2; x < SCREEN_W; x++) row[x] = dk;
        }
      } else {
        uint16_t f = (uint16_t)((wbFields[g_wbField] >> 8) | (wbFields[g_wbField] << 8));
        for (int y = 0; y < SCREEN_H; y++) {
          uint16_t* row = bufs[idx] + (size_t)y * SCREEN_W;
          for (int x = 0; x < SCREEN_W; x++) row[x] = f;
        }
      }
    } else if (s == 2) {
      cyberFrame(bufs[idx], SCREEN_W, SCREEN_H);
    } else {
      renderUiScene(s, bufs[idx], SCREEN_W, SCREEN_H);
    }
    // Top-band flicker guard (2026-09-19): with offset_rotation 2, buffer row 0
    // lands on the ST7789's last RAM row (319), which refreshes with a per-scan
    // luminance quirk (visible as a slow hazy band at the glass top edge).
    // Mirror row 0 from row 1 so the quirk row's content is identical to its
    // neighbor -> its ~5% modulation is imperceptible. (Both scenes already have
    // pure background there.)
    // TEST-C: guard memcpy temporarily disabled (crash-bisect 2026-09-19).
    // memcpy(bufs[idx], bufs[idx] + SCREEN_W, SCREEN_W * sizeof(uint16_t));
    g_renderUs = micros() - t;
    xQueueSend(readyQ, &idx, portMAX_DELAY);
  }
}

void cycleScene() {
  g_scene = (g_scene + 1) % sceneCount();
  g_lastSceneChange = millis();
  showLed(g_scene);
  Serial.printf("[tick] scene -> %d (%s)\n", g_scene, sceneName(g_scene));
}

// BOOT button: debounced falling edge -> next scene.
bool lastBtn = HIGH;
uint32_t lastBtnMs = 0;
void checkButton() {
  bool b = digitalRead(PIN_BTN);
  if (b != lastBtn && (millis() - lastBtnMs) > 40) {
    lastBtnMs = millis();
    lastBtn = b;
    if (b == LOW) cycleScene();
  }
}

// Serial test hook: a "PRESS" line over USB-serial emulates a BOOT button press.
// DIAGNOSTIC (2026-09-19): also "LED 0|1", "BL 0|25|50|100", "PAT 0..6", "ST".
void checkSerialCmd() {
  static char buf[16];
  static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (n > 0) buf[n] = 0;   // terminate so numeric parses can't read stale bytes
      if (n >= 5 && strncmp(buf, "PRESS", 5) == 0) cycleScene();
      else if (n >= 4 && strncmp(buf, "LED", 3) == 0) {
        if (atoi(buf + 4) == 0) {
          led.setPixelColor(0, 0); led.show(); g_diagLed = 0;
          Serial.println("[diag] LED off");
        } else {
          g_diagLed = 1; showLed(g_scene);
          Serial.println("[diag] LED on");
        }
      }
      else if (n >= 3 && strncmp(buf, "BL", 2) == 0) {
        int v = atoi(buf + 3);
        if (n >= 4 && strncmp(buf, "BLX", 3) == 0) {
          // channel probe: rows are [48, 46, 47]
          static const uint8_t t[7][3] = {
            { 1, 1, 1 }, { 1, 0, 1 }, { 1, 1, 0 }, { 1, 0, 0 },
            { 0, 0, 1 }, { 0, 1, 0 }, { 0, 0, 0 } };
          if (v <= 6) {
            g_diagBlDuty = 100;   // probe states are plain HIGH/LOW, full strength
            digitalWrite(PIN_BL, t[v][0]);
            digitalWrite(46, t[v][1]);
            digitalWrite(47, t[v][2]);
            Serial.printf("[diag] BLX %d (48=%d 46=%d 47=%d)\n", v, t[v][0], t[v][1], t[v][2]);
          }
        } else {
          g_diagBlDuty = (v == 0 || v == 25 || v == 50 || v == 100) ? v : 100;
          digitalWrite(PIN_BL, g_diagBlDuty > 0 ? HIGH : LOW);  // no PWM task on this build
          Serial.printf("[diag] BL duty=%d\n", g_diagBlDuty);
        }
      }
      else if (n >= 4 && strncmp(buf, "PAT", 3) == 0) {
        int v = atoi(buf + 4);
        g_diagPat = (v >= 0 && v <= 6) ? v : 0;
        Serial.printf("[diag] PAT %d\n", g_diagPat);
      }
      else if (n >= 4 && strncmp(buf, "WB", 2) == 0) {
        int v = atoi(buf + 2);
        g_wbField = (v >= 0 && v <= 7) ? v : 0;
        Serial.printf("[diag] WB %d\n", g_wbField);
      }
      else if (n >= 2 && strncmp(buf, "ST", 2) == 0) {
        Serial.printf("[diag] scene=%d(%s) blDuty=%d led=%d pat=%d render=%lums\n",
                      g_scene, sceneName(g_scene), g_diagBlDuty, g_diagLed, g_diagPat,
                      (unsigned long)(g_renderUs / 1000));
      }
      n = 0;
    } else if (n < (int)sizeof(buf) - 1) {
      buf[n++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[llm-tick] booting");

  bool ok = lcd.init();
  lcd.setRotation(0);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);                // backlight on (GPIO48 per on-unit blink test)
  pinMode(46, OUTPUT); digitalWrite(46, HIGH);  // unknown channels — known-HIGH baseline
  pinMode(47, OUTPUT); digitalWrite(47, HIGH);  // (2026-09-19: active-LOW backlight hunt)
  { // TEST-B: blTask temporarily disabled (crash-bisect 2026-09-19).
    // TaskHandle_t bt = nullptr;
    // BaseType_t rc = xTaskCreatePinnedToCore(blTask, "blpwm", 4096, nullptr, 3, &bt, 1);
    // Serial.printf("[tick] blTask rc=%d\n", (int)rc);
  }
  led.begin();

  uiWbInit();          // white-balance the UI palette for this backlight (before frame 1)
  cyberInit();         // cyberpunk scene palette + sub-scene state

  pinMode(PIN_BTN, INPUT_PULLUP);

  // ALLOCATION ORDER MATTERS. Create the queues + render task FIRST, while the
  // heap is still contiguous, then the two PSRAM framebuffers.
  freeQ  = xQueueCreate(2, sizeof(int));
  readyQ = xQueueCreate(2, sizeof(int));
  TaskHandle_t rt = nullptr;
  BaseType_t rc = xTaskCreatePinnedToCore(renderTask, "render", 16384, nullptr, 1, &rt, 0);
  Serial.printf("[tick] renderTask rc=%d\n", (int)rc);
  if (rc != pdPASS) { for (;;) delay(1000); }

  for (int i = 0; i < 2; i++) {
    sprites[i]->setPsram(true);
    sprites[i]->setColorDepth(16);
    bufs[i] = (uint16_t*)sprites[i]->createSprite(SCREEN_W, SCREEN_H);
  }
  Serial.printf("[tick] init=%s buf0=%s buf1=%s freeHeap=%u\n",
                ok ? "ok" : "FAIL", bufs[0] ? "OK" : "NULL", bufs[1] ? "OK" : "NULL",
                (unsigned)ESP.getFreeHeap());
  if (!bufs[0] || !bufs[1]) { for (;;) delay(1000); }

  wifiInit();
  ntpWait();
  MDNS.begin("llm-tick");
  resolveServer();
  fetchUsage();
  refreshWeather();

  for (int i = 0; i < 2; i++) xQueueSend(freeQ, &i, 0);
  showLed(g_scene);
  Serial.println("[tick] running — PRESS (serial) cycles usage <-> weather <-> cyberpunk");
}

void loop() {
  static uint32_t t0 = 0, fps_n = 0;

  checkButton();
  checkSerialCmd();

  int idx;
  if (xQueueReceive(readyQ, &idx, portMAX_DELAY) == pdTRUE) {
    sprites[idx]->pushSprite(0, 0);
    xQueueSend(freeQ, &idx, 0);
    fps_n++;
  }

  tickLogic();   // data poll cadence, idle->standby, weather refresh, wifi backstop

  if (millis() - t0 > 5000) {
    Serial.printf("[tick] scene=%d (%s) pushed=%lu render=%lums\n",
                  g_scene, sceneName(g_scene), (unsigned long)fps_n,
                  (unsigned long)(g_renderUs / 1000));
    fps_n = 0; t0 = millis();
  }
  delay(50);
}
