// weather_api.h — WeatherAPI.com client (ported from icefox0801/Tiny-Board,
// trimmed to the fields the standby screen shows).
#ifndef WEATHER_API_H
#define WEATHER_API_H
#include <HTTPClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "secrets.h"

struct WeatherData {
  String condition;   // human-readable, e.g. "Partly cloudy"
  int condition_code; // WeatherAPI code (maps to an ASCII icon in ui.cpp)
  float temperature;  // current
  float temp_low, temp_high;
  int humidity;       // %
  int aqi;            // US EPA index, 0 = n/a
  bool is_day;        // WeatherAPI current.is_day (drives the dynamic background)
  bool valid;
};

class WeatherAPI {
public:
  bool fetch();              // one HTTP call; false on any failure
  WeatherData get();
  bool needsUpdate(unsigned long ms = 3600000); // 1h
private:
  WeatherData _d{};
  unsigned long _last = 0;
};
#endif
