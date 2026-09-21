// board.h — Waveshare ESP32-S3-LCD-1.47B hardware truth (single source).
// Everything below was re-verified on the physical unit (2026-09-17, on-board
// camera + serial): the BACKLIGHT pin is unit-dependent (see PIN_BL note) and
// the glass is CENTERED in the controller RAM (offset_x 34, not 0 — the
// 1.47B schematic's 46/0-0 assumptions do not hold on this unit).
// Drive PIN_BL HIGH after init (it defaults OFF via a 10K gate pulldown).
// LovyanGFX Light_PWM is intentionally NOT used: its LEDC path is broken on
// esp32 core 3.x, so the backlight is a plain digitalWrite.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// --- Pins (1.47B) -------------------------------------------------------------
// BL: the 1.47B schematic says 46, but the blink matrix on THIS unit (2026-09-17)
// lights the backlight ONLY with GPIO48 driven HIGH (46 and 47 do nothing).
// This unit behaves like the base 1.47 — trust the hardware over the PDF.
#define PIN_BL   48   // LCD backlight, active HIGH (per on-unit blink test)
#define PIN_BTN  0    // wiki 1.47B puts a BOOT button here — THIS UNIT HAS NONE
                      // (on-unit check 2026-09-19): the poll is inert (nothing
                      // ever pulls it low); scene cycling is the serial PRESS line
#define PIN_RGB  38   // onboard WS2812 RGB LED

// --- Screen -------------------------------------------------------------------
static const int SCREEN_W = 172;
static const int SCREEN_H = 320;

// --- Verified ST7789 panel config ----------------------------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
public:
  LGFX() {
    { auto cfg = _bus.config();
      cfg.spi_host = SPI3_HOST; cfg.spi_mode = 0;
      cfg.freq_write = 80000000; cfg.freq_read = 16000000;
      cfg.spi_3wire = false; cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 40; cfg.pin_mosi = 45; cfg.pin_miso = -1; cfg.pin_dc = 41;
      _bus.config(cfg); _panel.setBus(&_bus); }
    { auto cfg = _panel.config();
      cfg.pin_cs = 42; cfg.pin_rst = 39; cfg.pin_busy = -1;
      // The 172-wide glass sits CENTERED in the ST7789's 240-wide RAM
      // (verified on this unit 2026-09-17: a full 172x320 @0,0 push left a
      // ~20%-wide stale strip at the glass edge; @34,0 covers it edge-to-edge).
      cfg.memory_width = 240; cfg.memory_height = 320;
      cfg.panel_width = 172;  cfg.panel_height = 320;
      // The glass is mounted 180° on this unit (content came up upside down
      // relative to the user's mount, verified 2026-09-17): flip the panel.
      // The centered 172-wide window is unaffected by the 180° flip, so the
      // @34,0 window position is unchanged.
      // NOTE (2026-09-19): the ST7789's LAST RAM row (319) refreshes with a
      // per-scan luminance quirk (gate-edge effect). With this unit's 180-deg
      // mount, rotation 2 puts row 319 at the visible TOP edge -> the perceived
      // "noise band". main.cpp guards against it by mirroring buffer row 0 from
      // row 1 so the quirk row's content matches its neighbor (imperceptible).
      cfg.offset_x = 34; cfg.offset_y = 0; cfg.offset_rotation = 2;
      cfg.dummy_read_pixel = 8; cfg.dummy_read_bits = 1; cfg.readable = false;
      cfg.invert = true; cfg.rgb_order = false; cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg); }
    setPanel(&_panel);
  }
};
