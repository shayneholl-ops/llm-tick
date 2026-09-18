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
    if (s < 2) {
      renderUiScene(s, bufs[idx], SCREEN_W, SCREEN_H);
    } else {
      Inputs in = { frame, g_ax, g_ay, g_az };
      EFFECTS[s - 2].fn(bufs[idx], SCREEN_W, SCREEN_H, in, PAL565[EFFECTS[s - 2].palette]);
    }
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
void checkSerialCmd() {
  static char buf[16];
  static int n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (n >= 5 && strncmp(buf, "PRESS", 5) == 0) cycleScene();
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
