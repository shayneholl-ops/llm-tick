// cyberpunk.h — the ambient "cyberpunk" scene (scene 2).
//
// Five procedural sub-scenes auto-cycle inside it (matrix rain, city skyline,
// glitch storm, hex-dump scroll, neural-net pulse), each ~9–14 s with a short
// black hold between. Concept: Oxpr0x/Waveshare-ESP32-S3-Cyberpunk-Display
// ("no SD" variant) — re-implemented for this unit's portrait 172x320 glass and
// drawn straight into llm-tick's framebuffer. No SD card, no TFT_eSPI, no extra
// libraries. See cyberpunk.cpp for the full provenance note.
#pragma once
#include <stdint.h>

// Initialise the palette + sub-scene state. Call once in setup() (the first
// cyberFrame() call paints the opening sub-scene, once a buffer is available).
void cyberInit();

// Render one frame of the currently active sub-scene into `buf` (w x h, raw
// RGB565 in the sprite's byte order, same layout as the UI scene).
void cyberFrame(uint16_t* buf, int w, int h);
