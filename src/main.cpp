#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
#include "board.h"
#include "effects.h"
#include "tick.h"

LGFX lcd;
LGFX_Sprite spr0(&lcd), spr1(&lcd);
LGFX_Sprite* sprites[2] = { &spr0, &spr1 };
LGFX_Sprite* uiSpr = &spr0;   // UI scene draws through this (into the active buffer)
uint16_t*    bufs[2]    = { nullptr, nullptr };
Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// A "scene" is 0 = usage, 1 = weather standby, 2..N-1 = genart effects.
// BOOT cycles through all of them with one counter.
volatile int   g_scene = 0;
int            sceneCount() { return 2 + NUM_EFFECTS; }
const char*    sceneName(int s) { return s < 2 ? (s == 0 ? "usage" : "weather") : EFFECTS[s - 2].name; }

volatile float g_ax = 0, g_ay = 0, g_az = 1;
volatile uint32_t g_renderUs = 0;

QueueHandle_t freeQ, readyQ;   // carry buffer indices (0/1) between the two cores

// ── DIAGNOSTIC (top-band flicker hunt, 2026-09-19) ────────────────────────────
// Extra serial commands (sent as one line each; see checkSerialCmd):
//   LED 0|1      WS2812 ring off / restore scene color
//   BL  0|25|50|100   backlight duty: off / 25% / 50% / full (1.5 kHz software PWM)
//   PAT 0..6     0=normal scenes; 1=mid gray; 2=white top 24 rows / black rest;
//                3=black top 24 / gray rest; 4=all black; 5=all white;
//                6=black top 24 + black rows 150-173 / gray rest (position control)
//   ST           print diagnostic state
volatile int g_diagLed = 1;
volatile int g_diagBlDuty = 100;   // 0 = off, 100 = full
volatile int g_diagPat = 0;

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
  if (s == 0) c = led.Color(0, 24, 24);        // usage: blue
  else if (s == 1) c = led.Color(0, 20, 10);   // standby: soft green
  else c = led.Color(EFFECTS[s - 2].ledR, EFFECTS[s - 2].ledG, EFFECTS[s - 2].ledB);
  led.setPixelColor(0, c);
  led.show();
}

// Producer (core 0): take a free buffer, render the active scene into it, hand
// it to the consumer. Effects write byte-swapped RGB565 directly; UI scenes draw
// through lcd into the same buffer (fonts are panel-bound on this core).
void renderTask(void*) {
  uint32_t frame = 0;
  for (;;) {
    int idx;
    xQueueReceive(freeQ, &idx, portMAX_DELAY);
    int s = g_scene;
    uint32_t t = micros();
    uiSpr = sprites[idx];   // UI scene draws into this frame's buffer
    if (g_diagPat != 0) {
      fillPattern(bufs[idx], SCREEN_W, SCREEN_H, g_diagPat);
    } else if (s < 2) {
      renderUiScene(s, bufs[idx], SCREEN_W, SCREEN_H);
    } else {
      Inputs in = { frame, g_ax, g_ay, g_az };
      EFFECTS[s - 2].fn(bufs[idx], SCREEN_W, SCREEN_H, in, PAL565[EFFECTS[s - 2].palette]);
    }
    // Top-band flicker guard (2026-09-19): with offset_rotation 2, buffer row 0
    // lands on the ST7789's last RAM row (319), which refreshes with a per-scan
    // luminance quirk (visible as a slow hazy band at the glass top edge).
    // Mirror row 0 from row 1 so the quirk row's content is identical to its
    // neighbor -> its ~5% modulation is imperceptible. (UI scenes already have
    // background there; this makes the effects consistent too.)
    // TEST-C: guard memcpy temporarily disabled (crash-bisect 2026-09-19).
    // memcpy(bufs[idx], bufs[idx] + SCREEN_W, SCREEN_W * sizeof(uint16_t));
    g_renderUs = micros() - t;
    frame++;
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
        g_diagBlDuty = (v == 0 || v == 25 || v == 50 || v == 100) ? v : 100;
        Serial.printf("[diag] BL duty=%d\n", g_diagBlDuty);
      }
      else if (n >= 4 && strncmp(buf, "PAT", 3) == 0) {
        int v = atoi(buf + 4);
        g_diagPat = (v >= 0 && v <= 6) ? v : 0;
        Serial.printf("[diag] PAT %d\n", g_diagPat);
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
  { // TEST-B: blTask temporarily disabled (crash-bisect 2026-09-19).
    // TaskHandle_t bt = nullptr;
    // BaseType_t rc = xTaskCreatePinnedToCore(blTask, "blpwm", 4096, nullptr, 3, &bt, 1);
    // Serial.printf("[tick] blTask rc=%d\n", (int)rc);
  }
  led.begin();

  buildTables();
  effectsSeed(esp_random());
  for (int p = 0; p < NUM_PALETTES; p++)
    for (int i = 0; i < 256; i++) {
      uint16_t c = lcd.color565(PALETTES[p][i][0], PALETTES[p][i][1], PALETTES[p][i][2]);
      PAL565[p][i] = (uint16_t)((c >> 8) | (c << 8));   // byte-swap for sprite layout
    }

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
  Serial.println("[tick] running — BOOT cycles usage -> weather -> effects");
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
