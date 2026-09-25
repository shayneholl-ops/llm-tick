// wxscene.h — the animated "Cascadia" weather background for the standby scene.
//
// Ported from the design export in stitch_animated_lvgl_weather_backgrounds/
// (a 172x320 vector mockup, coordinates 1:1 with this glass):
//   day   — North Shore coast: 3-stop sky, sun glow, drifting mist, the
//           Lions + Grouse silhouettes, swaying conifers, animated sea waves,
//           a Lions Gate bridge line accent.
//   night — Burrard Inlet nocturne: darker sky, undulating aurora ribbon,
//           twinkling stars, crescent moon, pine ridge, bridge with
//           reflections.
// Weather data picks the particles: rain drizzle (4.2 s), snow flurry (8.5 s),
// wind gusts (7 s), clear shimmers/stars (5 s twinkle) — all "calmed, subtle
// and slower" per the design. Concept only: every element is re-implemented
// here, drawn straight into llm-tick's framebuffer. No assets, no libraries.
#pragma once
#include <stdint.h>

// Condition families (shared with ui.cpp's icon/scene mapping).
enum WxFam {
  WX_SUN, WX_MOON, WX_PARTLY_D, WX_PARTLY_N, WX_CLOUD, WX_RAIN, WX_SNOW, WX_STORM, WX_FOG
};

// Build the panel-ready palette (wb565 + byte swap). Call once in setup().
void wxSceneInit();

// Render one frame of the animated background into `buf` (w x h).
//   fam   — the condition family (drives which particles animate)
//   day   — true = coast scene, false = aurora nocturne (is_day / WXN force)
//   valid — false draws the flat standby canvas instead
void wxSceneRender(uint16_t* buf, int w, int h, int fam, bool day, bool valid);

// The scene's logical (unswapped) colour at row y — sampled from the last
// rendered frame at x==2. Used for legibility checks and for the UI's 1-px
// header rule; NOT as a text background (the UI draws transparent over the
// scene — see the note on bgAt in ui.cpp).
uint16_t wxRowColor(int y);
