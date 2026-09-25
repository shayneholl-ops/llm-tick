// Host stub of the only two things wxscene.cpp takes from the real tick.h.
#pragma once
#include <cstdint>

static inline uint16_t wb565(uint16_t c) {
  int r = (c >> 11) & 31;
  int g = ((c >> 5) & 63) * 14 / 16;
  int b = (c & 31) * 10 / 16;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
