// Desk dashboard: clock + weather (Celsius) + touch pages + stocks + night mode + OTA updates
// Board: Guition JC2432W328 (ESP32 Dev Module)
// Libraries: LovyanGFX, ArduinoJson (v7)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "lgfx_jc2432w328.h"
#include "secrets.h"
#ifndef FINNHUB_KEY
#define FINNHUB_KEY ""
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// ---------------- Settings ----------------
const float LAT = 37.41f;                       // North San Jose (95134)
const float LON = -121.95f;
const char* TZ_INFO = "PST8PDT,M3.2.0,M11.1.0"; // Pacific time with DST rules
const unsigned long WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL;  // every 10 min
const unsigned long WEATHER_RETRY_MS    = 60UL * 1000UL;         // retry after 1 min on failure
const unsigned long PAGE_TIMEOUT_MS     = 30UL * 1000UL;         // return to home after 30 s
const bool DEBUG_TOUCH = false;  // true = draw a dot where you tap and pause 300 ms

// ---------------- Night mode ----------------
const int NIGHT_START_HOUR = 23;          // 10 PM
const int NIGHT_END_HOUR   = 7;           // 7 AM
const int DAY_BRIGHTNESS   = 180;         // 0-255
const int NIGHT_BRIGHTNESS = 10;
const unsigned long WAKE_MS = 15UL * 1000UL;  // a tap at night brightens for 15 s

// ---------------- OTA (updates over Wi-Fi) ----------------
const char* OTA_HOSTNAME = "desk-dashboard";  // reachable as desk-dashboard.local

// ---------------- Stocks ----------------
const char* TICKERS[] = {"AAPL", "NVDA", "MRVL", "ALAB"};
const int NUM_TICKERS = sizeof(TICKERS) / sizeof(TICKERS[0]);
const unsigned long STOCKS_OPEN_MS   = 60UL * 1000UL;        // every minute while market is open
const unsigned long STOCKS_CLOSED_MS = 30UL * 60UL * 1000UL; // every 30 min otherwise

// ---------------- Touch (CST820 capacitive, I2C) ----------------
const int     TOUCH_RST  = 25;
const uint8_t TOUCH_ADDR = 0x15;
int touchSda = 33, touchScl = 32;   // swapped automatically if not found

// ---------------- Colours (RGB565) ----------------
const uint16_t COL_BG    = 0x0862;   // deep navy
const uint16_t COL_CARD  = 0x10E6;   // slightly lighter panel
const uint16_t COL_TEXT  = TFT_WHITE;
const uint16_t COL_DIM   = 0x9CF3;   // soft grey
const uint16_t COL_HIGH  = 0xFD20;   // orange
const uint16_t COL_LOW   = 0x867D;   // sky blue
const uint16_t COL_SUN   = 0xFE60;   // warm yellow
const uint16_t COL_CLOUD = 0xC618;   // light grey
const uint16_t COL_RAIN  = 0x5D1C;   // blue
const uint16_t COL_UP    = 0x262B;   // green
const uint16_t COL_DOWN  = 0xEA28;   // red

LGFX tft;

struct Weather {
  bool  valid = false;
  float temp = 0, feels = 0, hi = 0, lo = 0, wind = 0;
  int   humidity = 0, code = 0;
  bool  isDay = true;
} wx;

enum Page { PAGE_HOME, PAGE_STOCKS, PAGE_CALENDAR, PAGE_COUNT };
int page = PAGE_HOME;

unsigned long nextWeatherFetch = 0;
unsigned long lastPageChange = 0;
unsigned long lastTouchPoll = 0;
unsigned long lastTapMs = 0;
bool touchOK = false;
bool wasPressed = false;
int lastMinute = -1;
char lastUpdated[12] = "";
int currentBrightness = DAY_BRIGHTNESS;
unsigned long wakeUntil = 0;
unsigned long lastFade = 0;
bool otaStarted = false;

struct Quote {
  bool  valid = false;
  float price = 0, change = 0, pct = 0;
};
Quote quotes[sizeof(TICKERS) / sizeof(TICKERS[0])];
unsigned long nextStocksFetch = 0;
char stocksUpdated[12] = "";

// ---------------- Helpers ----------------
const char* weatherText(int c) {
  switch (c) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Frz. drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Frz. rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "T-storm, hail";
    default: return "Unknown";
  }
}

bool haveFinnhubKey() {
  return strlen(FINNHUB_KEY) > 0 && strcmp(FINNHUB_KEY, "your-finnhub-key") != 0;
}

// ---------------- Night mode ----------------
bool isNight() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return false;
  int h = t.tm_hour;
  if (NIGHT_START_HOUR > NIGHT_END_HOUR) return h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR;
  return h >= NIGHT_START_HOUR && h < NIGHT_END_HOUR;
}

bool awake() { return (long)(wakeUntil - millis()) > 0; }

// Fade the backlight toward its target a few steps at a time (smooth, non-blocking)
void updateBrightness() {
  if (millis() - lastFade < 15) return;
  lastFade = millis();
  int target = (isNight() && !awake()) ? NIGHT_BRIGHTNESS : DAY_BRIGHTNESS;
  if (currentBrightness == target) return;
  int step = 6;
  if (target > currentBrightness) currentBrightness = min(target, currentBrightness + step);
  else                            currentBrightness = max(target, currentBrightness - step);
  tft.setBrightness(currentBrightness);
}

// US market hours in Pacific time: Mon-Fri 6:30 AM - 1:00 PM (holidays ignored)
bool marketOpen() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return true;
  if (t.tm_wday == 0 || t.tm_wday == 6) return false;
  int m = t.tm_hour * 60 + t.tm_min;
  return m >= 390 && m < 780;
}

// "1:05 PM" style timestamp
void nowString(char* out, size_t len) {
  struct tm t;
  out[0] = 0;
  if (!getLocalTime(&t, 0)) return;
  strftime(out, len, "%I:%M %p", &t);
  if (out[0] == '0') memmove(out, out + 1, strlen(out));
}

void showMessage(const char* msg, uint16_t color = COL_TEXT) {
  tft.fillScreen(COL_BG);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(middle_center);
  tft.setTextColor(color, COL_BG);
  tft.drawString(msg, 160, 120);
}

// ---------------- Touch driver ----------------
bool touchProbe(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl, 400000);
  Wire.beginTransmission(TOUCH_ADDR);
  return Wire.endTransmission() == 0;
}

bool touchInit() {
  // Reset pulse wakes the controller (it ignores I2C while asleep)
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(60);

  if (!touchProbe(touchSda, touchScl)) {
    int t = touchSda; touchSda = touchScl; touchScl = t;     // try swapped pins
    if (!touchProbe(touchSda, touchScl)) return false;
  }
  // Disable auto-sleep so it always answers
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0xFE);
  Wire.write(0x01);
  Wire.endTransmission();
  Serial.printf("Touch: CST820 found at 0x15 (SDA %d, SCL %d)\n", touchSda, touchScl);
  return true;
}

// Returns true while a finger is down; x/y in landscape screen coordinates
bool touchRead(int& x, int& y, int& rawX, int& rawY) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);                                  // finger count register
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_ADDR, (uint8_t)5) != 5) return false;
  uint8_t fingers = Wire.read();
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  if (fingers == 0) return false;

  rawX = ((xh & 0x0F) << 8) | xl;   // portrait: 0..239
  rawY = ((yh & 0x0F) << 8) | yl;   // portrait: 0..319
  // Portrait -> landscape (rotation 1). If the debug dot is mirrored, fix here.
  x = rawY;
  y = 239 - rawX;
  return true;
}

// ---------------- Weather icons (drawn with shapes) ----------------
void drawSun(int cx, int cy, int r) {
  for (int a = 0; a < 360; a += 45) {
    float rad = a * DEG_TO_RAD;
    tft.drawLine(cx + cos(rad) * (r + 4), cy + sin(rad) * (r + 4),
                 cx + cos(rad) * (r + 10), cy + sin(rad) * (r + 10), COL_SUN);
  }
  tft.fillCircle(cx, cy, r, COL_SUN);
}

void drawMoon(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, COL_CLOUD);
  tft.fillCircle(cx + r / 2, cy - r / 3, r - 2, COL_CARD);
}

void drawCloud(int cx, int cy, uint16_t c) {
  tft.fillCircle(cx - 14, cy + 4, 12, c);
  tft.fillCircle(cx + 2,  cy - 5, 16, c);
  tft.fillCircle(cx + 18, cy + 5, 11, c);
  tft.fillRoundRect(cx - 26, cy + 4, 56, 13, 6, c);
}

void drawIcon(int code, bool isDay, int cx, int cy) {
  tft.fillRect(cx - 40, cy - 40, 80, 84, COL_CARD);
  if (code <= 1) {
    if (isDay) drawSun(cx, cy, 16); else drawMoon(cx, cy, 18);
  } else if (code == 2) {
    if (isDay) drawSun(cx + 10, cy - 12, 12); else drawMoon(cx + 10, cy - 12, 13);
    drawCloud(cx - 2, cy + 6, COL_CLOUD);
  } else if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++) tft.fillRoundRect(cx - 28 + (i % 2) * 6, cy - 14 + i * 10, 50, 4, 2, COL_CLOUD);
  } else {
    drawCloud(cx, cy - 6, COL_CLOUD);
    bool rain  = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    bool snow  = (code >= 71 && code <= 77) || code == 85 || code == 86;
    bool storm = code >= 95;
    if (rain) {
      for (int i = -1; i <= 1; i++) tft.drawWideLine(cx + i * 14, cy + 18, cx + i * 14 - 5, cy + 30, 2, COL_RAIN);
    } else if (snow) {
      for (int i = -1; i <= 1; i++) tft.fillCircle(cx + i * 14, cy + 24, 3, TFT_WHITE);
    } else if (storm) {
      tft.fillTriangle(cx + 2, cy + 12, cx - 8, cy + 28, cx + 2, cy + 26, COL_SUN);
      tft.fillTriangle(cx - 2, cy + 22, cx + 8, cy + 22, cx - 4, cy + 38, COL_SUN);
    }
  }
}

void drawDegreeC(int x, int yCircle, int yLetter, uint16_t col, uint16_t bg) {
  tft.drawCircle(x + 4, yCircle, 4, col);
  tft.drawCircle(x + 4, yCircle, 3, col);
  tft.setFont(&fonts::FreeSansBold12pt7b);
  tft.setTextColor(col, bg);
  tft.drawString("C", x + 11, yLetter);
}

// ---------------- Screen sections ----------------
void drawStatusBar() {
  tft.fillRect(0, 0, 320, 22, COL_BG);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(top_left);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("North San Jose", 10, 4);

  // Page dots
  for (int i = 0; i < PAGE_COUNT; i++) {
    int cx = 150 + i * 12;
    if (i == page) tft.fillCircle(cx, 11, 3, COL_TEXT);
    else           tft.drawCircle(cx, 11, 3, COL_DIM);
  }

  tft.setTextDatum(top_right);
  tft.setTextColor(COL_DIM, COL_BG);
  if (WiFi.status() != WL_CONNECTED) {
    tft.setTextColor(TFT_RED, COL_BG);
    tft.drawString("Wi-Fi offline", 310, 4);
  } else if (!wx.valid) {
    tft.drawString("Getting weather...", 310, 4);
  } else {
    char buf[32];
    if (lastUpdated[0]) snprintf(buf, sizeof(buf), "Updated %s", lastUpdated);
    else                snprintf(buf, sizeof(buf), "Weather OK");
    tft.drawString(buf, 310, 4);
  }
}

void drawClock(const struct tm* t) {
  tft.fillRect(0, 24, 320, 96, COL_BG);

  char hm[8];
  if (t) strftime(hm, sizeof(hm), "%I:%M", t);
  else   strcpy(hm, "--:--");
  const char* hmStr = (hm[0] == '0') ? hm + 1 : hm;   // 09:05 -> 9:05

  tft.setTextDatum(top_left);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setFont(&fonts::Font7);                          // 7-segment digits
  tft.drawString(hmStr, 14, 32);
  int x = 14 + tft.textWidth(hmStr) + 8;

  tft.setFont(&fonts::Font4);
  if (t) {
    char ampm[4], date[32];
    strftime(ampm, sizeof(ampm), "%p", t);
    strftime(date, sizeof(date), "%A, %b ", t);
    snprintf(date + strlen(date), sizeof(date) - strlen(date), "%d", t->tm_mday);
    tft.drawString(ampm, x, 34);
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(date, 16, 90);
  } else {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Syncing time...", 16, 90);
  }
}

void drawWeather() {
  tft.fillRoundRect(8, 124, 304, 110, 10, COL_CARD);
  if (!wx.valid) {
    tft.setFont(&fonts::Font4);
    tft.setTextDatum(middle_center);
    tft.setTextColor(COL_DIM, COL_CARD);
    tft.drawString("Waiting for weather...", 160, 179);
    return;
  }

  drawIcon(wx.code, wx.isDay, 54, 176);

  // Big temperature
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%d", (int)roundf(wx.temp));
  tft.setTextDatum(top_left);
  tft.setTextColor(COL_TEXT, COL_CARD);
  tft.setFont(&fonts::FreeSansBold24pt7b);
  tft.drawString(tbuf, 104, 134);
  drawDegreeC(104 + tft.textWidth(tbuf) + 6, 142, 137, COL_TEXT, COL_CARD);

  // Condition and feels-like
  tft.setFont(&fonts::FreeSans9pt7b);
  tft.setTextColor(COL_DIM, COL_CARD);
  tft.drawString(weatherText(wx.code), 106, 186);
  char fbuf[24];
  snprintf(fbuf, sizeof(fbuf), "Feels like %dC", (int)roundf(wx.feels));
  tft.setFont(&fonts::Font2);
  tft.drawString(fbuf, 106, 208);

  // Right column: high, low, humidity, wind
  char buf[20];
  tft.setTextColor(COL_HIGH, COL_CARD);
  snprintf(buf, sizeof(buf), "H %dC", (int)roundf(wx.hi));
  tft.drawString(buf, 244, 136);
  tft.setTextColor(COL_LOW, COL_CARD);
  snprintf(buf, sizeof(buf), "L %dC", (int)roundf(wx.lo));
  tft.drawString(buf, 244, 158);
  tft.setTextColor(COL_DIM, COL_CARD);
  snprintf(buf, sizeof(buf), "%d%% hum", wx.humidity);
  tft.drawString(buf, 244, 186);
  snprintf(buf, sizeof(buf), "%d mph", (int)roundf(wx.wind));
  tft.drawString(buf, 244, 208);
}

void drawPlaceholderPage(const char* title, const char* subtitle) {
  tft.fillRoundRect(8, 30, 304, 204, 10, COL_CARD);
  tft.setTextDatum(middle_center);
  tft.setTextColor(COL_TEXT, COL_CARD);
  tft.setFont(&fonts::FreeSansBold18pt7b);
  tft.drawString(title, 160, 110);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(COL_DIM, COL_CARD);
  tft.drawString(subtitle, 160, 150);
  tft.drawString("Tap left / right to switch pages", 160, 200);
}

void drawStocks() {
  tft.fillRoundRect(8, 26, 304, 188, 10, COL_CARD);
  tft.fillRect(0, 216, 320, 24, COL_BG);

  if (!haveFinnhubKey()) {
    tft.setTextDatum(middle_center);
    tft.setFont(&fonts::Font2);
    tft.setTextColor(COL_DIM, COL_CARD);
    tft.drawString("Add FINNHUB_KEY to secrets.h", 160, 110);
    tft.drawString("(free key from finnhub.io)", 160, 132);
    return;
  }

  for (int i = 0; i < NUM_TICKERS; i++) {
    int yc = 49 + i * 46;
    if (i > 0) tft.drawFastHLine(20, yc - 23, 280, COL_BG);

    tft.setTextDatum(middle_left);
    tft.setFont(&fonts::FreeSansBold12pt7b);
    tft.setTextColor(COL_TEXT, COL_CARD);
    tft.drawString(TICKERS[i], 22, yc - 6);

    const Quote& q = quotes[i];
    if (!q.valid) {
      tft.setTextDatum(middle_right);
      tft.setFont(&fonts::Font2);
      tft.setTextColor(COL_DIM, COL_CARD);
      tft.drawString("loading...", 300, yc);
      continue;
    }

    uint16_t col = (q.change >= 0) ? COL_UP : COL_DOWN;
    char buf[24];

    // Absolute change under the ticker
    snprintf(buf, sizeof(buf), "%+.2f", q.change);
    tft.setFont(&fonts::Font2);
    tft.setTextColor(col, COL_CARD);
    tft.drawString(buf, 22, yc + 13);

    // Price
    snprintf(buf, sizeof(buf), "%.2f", q.price);
    tft.setTextDatum(middle_right);
    tft.setFont(&fonts::FreeSans12pt7b);
    tft.setTextColor(COL_TEXT, COL_CARD);
    tft.drawString(buf, 212, yc);

    // Percent change pill
    tft.fillRoundRect(222, yc - 13, 82, 26, 7, col);
    snprintf(buf, sizeof(buf), "%+.2f%%", q.pct);
    tft.setTextDatum(middle_center);
    tft.setFont(&fonts::FreeSansBold9pt7b);
    tft.setTextColor(TFT_WHITE, col);
    tft.drawString(buf, 263, yc);
  }

  // Footer: market state + last update
  char foot[48];
  snprintf(foot, sizeof(foot), "%s%s%s",
           marketOpen() ? "Market open" : "Market closed",
           stocksUpdated[0] ? "  -  Updated " : "", stocksUpdated);
  tft.setTextDatum(middle_center);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString(foot, 160, 228);
}

void showPage(int p) {
  page = p;
  lastPageChange = millis();
  tft.fillScreen(COL_BG);
  drawStatusBar();
  if (page == PAGE_HOME) {
    struct tm t;
    drawClock(getLocalTime(&t, 0) ? &t : nullptr);
    drawWeather();
  } else if (page == PAGE_STOCKS) {
    drawStocks();
  } else {
    drawPlaceholderPage("Calendar", "Coming in step 5");
  }
  Serial.printf("Page -> %d\n", page);
}

// ---------------- Network ----------------
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  char url[420];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
    "&daily=temperature_2m_max,temperature_2m_min"
    "&temperature_unit=celsius&wind_speed_unit=mph"
    "&timezone=America%%2FLos_Angeles&forecast_days=1",
    LAT, LON);

  WiFiClientSecure client;
  client.setInsecure();   // skip certificate check: fine for public weather data
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.println("Weather: http.begin failed");
    return false;
  }
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    Serial.printf("Weather: HTTP error %d\n", status);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Weather: JSON error %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  wx.temp     = cur["temperature_2m"].as<float>();
  wx.feels    = cur["apparent_temperature"].as<float>();
  wx.humidity = cur["relative_humidity_2m"].as<int>();
  wx.code     = cur["weather_code"].as<int>();
  wx.wind     = cur["wind_speed_10m"].as<float>();
  wx.isDay    = cur["is_day"].as<int>() == 1;
  wx.hi       = doc["daily"]["temperature_2m_max"][0].as<float>();
  wx.lo       = doc["daily"]["temperature_2m_min"][0].as<float>();
  wx.valid    = true;

  struct tm t;
  if (getLocalTime(&t, 0)) {
    strftime(lastUpdated, sizeof(lastUpdated), "%I:%M %p", &t);
    if (lastUpdated[0] == '0') memmove(lastUpdated, lastUpdated + 1, strlen(lastUpdated));
  }
  Serial.printf("Weather: %.1fC, %s, H %.0f / L %.0f, %d%% hum, %.0f mph\n",
                wx.temp, weatherText(wx.code), wx.hi, wx.lo, wx.humidity, wx.wind);
  return true;
}

bool fetchStocks() {
  if (WiFi.status() != WL_CONNECTED || !haveFinnhubKey()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(true);        // keep one TLS connection open for all tickers
  http.setTimeout(10000);

  bool any = false;
  for (int i = 0; i < NUM_TICKERS; i++) {
    char url[160];
    snprintf(url, sizeof(url), "https://finnhub.io/api/v1/quote?symbol=%s&token=%s",
             TICKERS[i], FINNHUB_KEY);
    if (!http.begin(client, url)) continue;
    int status = http.GET();
    if (status == HTTP_CODE_OK) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString())) {
        float c = doc["c"] | 0.0f;      // current price
        if (c > 0) {
          quotes[i].price  = c;
          quotes[i].change = doc["d"]  | 0.0f;   // change vs previous close
          quotes[i].pct    = doc["dp"] | 0.0f;   // percent change
          quotes[i].valid  = true;
          any = true;
          Serial.printf("Stocks: %-5s %8.2f  %+.2f (%+.2f%%)\n",
                        TICKERS[i], c, quotes[i].change, quotes[i].pct);
        }
      }
    } else {
      Serial.printf("Stocks: %s HTTP error %d\n", TICKERS[i], status);
    }
    http.end();
  }
  if (any) nowString(stocksUpdated, sizeof(stocksUpdated));
  return any;
}

// ---------------- OTA ----------------
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    wakeUntil = millis() + 10UL * 60UL * 1000UL;
    currentBrightness = DAY_BRIGHTNESS;
    tft.setBrightness(DAY_BRIGHTNESS);
    showMessage("Updating over Wi-Fi...");
    tft.drawRect(40, 150, 240, 14, COL_DIM);
    Serial.println("OTA: update started");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    tft.fillRect(42, 152, (236UL * done) / total, 10, COL_UP);
  });
  ArduinoOTA.onEnd([]() {
    showMessage("Update done, restarting");
    Serial.println("OTA: done");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA: error %u\n", e);
    showPage(page);
  });
  ArduinoOTA.begin();
  Serial.printf("OTA: ready at %s.local (%s)\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);          // landscape 320x240
  tft.setBrightness(DAY_BRIGHTNESS);

  touchOK = touchInit();
  if (!touchOK) {
    Serial.println("Touch: no CST820 at 0x15 on pins 32/33. Board may be the resistive (R) variant.");
    showMessage("Touch not found", TFT_ORANGE);
    delay(2000);
  }

  showMessage("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected, IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi failed. Check secrets.h (2.4 GHz networks only).");
    showMessage("Wi-Fi failed: check secrets.h", TFT_RED);
    delay(3000);
  }

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  showPage(PAGE_HOME);
}

void loop() {
  unsigned long now = millis();

  // OTA: start once Wi-Fi is up, then service it every pass
  if (!otaStarted && WiFi.status() == WL_CONNECTED) {
    setupOTA();
    otaStarted = true;
  }
  if (otaStarted) ArduinoOTA.handle();

  updateBrightness();

  // Touch: poll every 20 ms, act on the press edge (finger down)
  if (touchOK && now - lastTouchPoll >= 20) {
    lastTouchPoll = now;
    int x, y, rx, ry;
    bool pressed = touchRead(x, y, rx, ry);
    if (pressed && !wasPressed && now - lastTapMs > 250) {
      lastTapMs = now;
      Serial.printf("Tap: raw (%d, %d) -> screen (%d, %d)\n", rx, ry, x, y);
      if (DEBUG_TOUCH) {
        tft.fillCircle(x, y, 8, TFT_MAGENTA);
        delay(300);
      }
      bool wasDim = isNight() && !awake();
      if (isNight()) wakeUntil = millis() + WAKE_MS;
      if (wasDim) {
        Serial.println("Night mode: wake");        // first tap only brightens the screen
      } else {
        int next = (x >= 160) ? (page + 1) % PAGE_COUNT
                              : (page + PAGE_COUNT - 1) % PAGE_COUNT;
        showPage(next);
      }
    }
    wasPressed = pressed;
  }

  // Return to the home page after a while
  // Use a fresh millis(): showPage() may have set lastPageChange after 'now' was sampled
  if (page != PAGE_HOME && millis() - lastPageChange > PAGE_TIMEOUT_MS) showPage(PAGE_HOME);

  // Weather: fire when the timer expires (like a counter-based enable)
  if ((long)(now - nextWeatherFetch) >= 0) {
    bool ok = fetchWeather();
    nextWeatherFetch = now + (ok ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS);
    if (ok && page == PAGE_HOME) drawWeather();
    drawStatusBar();
  }

  // Stocks: every minute while the market is open, every 30 min otherwise
  if ((long)(millis() - nextStocksFetch) >= 0) {
    bool ok = fetchStocks();
    nextStocksFetch = millis() + (ok ? (marketOpen() ? STOCKS_OPEN_MS : STOCKS_CLOSED_MS)
                                     : WEATHER_RETRY_MS);
    if (page == PAGE_STOCKS) drawStocks();
  }

  // Clock: redraw only when the minute changes
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_min != lastMinute) {
    lastMinute = t.tm_min;
    if (page == PAGE_HOME) drawClock(&t);
    drawStatusBar();
  }

  delay(5);
}
