// Host stub so wxscene.cpp can be compiled for an offline framebuffer preview.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>

extern uint32_t g_hostMillis;
static inline uint32_t millis() { return g_hostMillis; }
