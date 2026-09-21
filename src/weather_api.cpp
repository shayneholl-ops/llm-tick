#include "weather_api.h"

bool WeatherAPI::fetch() {
  if (WiFi.status() != WL_CONNECTED || WEATHER_API_KEY[0] == '\0') return false;

  String url = "http://api.weatherapi.com/v1/forecast.json?key=" + String(WEATHER_API_KEY)
             + "&q=" + String(WEATHER_LOCATION) + "&days=1&aqi=yes&alerts=no";
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.begin(url);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  if (!doc["current"].is<JsonObject>()) return false;

  _d = {};
  _d.temperature    = doc["current"]["temp_c"];
  _d.humidity       = doc["current"]["humidity"] | 0;
  _d.condition_code = doc["current"]["condition"]["code"] | 0;
  String cond       = doc["current"]["condition"]["text"].as<String>();
  _d.condition      = (cond.length() > 18) ? cond.substring(0, 18) : cond;
  _d.is_day         = (doc["current"]["is_day"] | 0) != 0;

  float hi = doc["forecast"]["forecastday"][0]["day"]["maxtemp_c"] | NAN;
  float lo = doc["forecast"]["forecastday"][0]["day"]["mintemp_c"] | NAN;
  if (hi == hi) _d.temp_high = hi;   // NaN check = "field present"
  if (lo == lo) _d.temp_low  = lo;
  if (doc["current"]["air_quality"].is<JsonObject>())
    _d.aqi = doc["current"]["air_quality"]["us-epa-index"] | 0;
  _d.valid = true;
  _last = millis();
  return true;
}

WeatherData WeatherAPI::get() { return _d; }

bool WeatherAPI::needsUpdate(unsigned long ms) {
  return _d.valid && (millis() - _last > ms);
}
