// Tote level monitor — Waveshare ESP32-C6-LCD-1.3 + 3x HC-SR04
// Standalone: shows each tote's water level on the screen, RGB LED shows the worst tote.
//
// Wiring (header pin numbers as printed on the board):
//   All HC-SR04 VCC -> 5V, all GND -> GND
//   T1  TRIG -> GPIO2    ECHO -> divider -> GPIO3
//   T2  TRIG -> GPIO20   ECHO -> divider -> GPIO23
//   T3  TRIG -> GPIO16   ECHO -> divider -> GPIO17
//   Divider: ECHO -> 1k -> GPIO, GPIO -> 2k -> GND
//
// Calibration: sensor face is 13.5 in above the tote floor (same on all three).
// Water depth = 13.5 in - sonar distance. Depth is turned into gallons with
// one table averaged from the T1/T2/T3 fill tests (see CAL_DEPTH_IN / CAL_GAL).

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ---------- Onboard hardware ----------
constexpr int PIN_LCD_MOSI = 6;
constexpr int PIN_LCD_SCLK = 7;
constexpr int PIN_LCD_CS   = 14;
constexpr int PIN_LCD_DC   = 15;
constexpr int PIN_LCD_RST  = 21;
constexpr int PIN_LCD_BL   = 22;
constexpr int PIN_RGB      = 8;

// ---------- Calibration ----------
constexpr float CM_PER_IN        = 2.54f;
constexpr float SENSOR_HEIGHT_IN = 13.5f;   // sensor face to tote floor
constexpr float TOTE_MAX_GAL     = 27.0f;   // physical tote size, hard cap

// Averaged across T1, T2, T3: water depth (in) -> gallons
const float CAL_DEPTH_IN[] = {0.0f, 3.27f, 5.92f, 8.56f, 10.79f};
const float CAL_GAL[]      = {0.0f, 5.0f,  10.0f, 15.0f, 20.0f};
constexpr int CAL_N = sizeof(CAL_GAL) / sizeof(CAL_GAL[0]);

// ---------- Settings ----------
constexpr float GAUGE_FULL_GAL = 20.0f;    // totes never run above 20 gal
constexpr float LOW_GAL      = 12.0f;      // red below this
constexpr float WATCH_MARGIN = 4.0f;       // amber from LOW_GAL to LOW_GAL + this
constexpr uint32_t READ_EVERY_MS = 5000;
constexpr uint32_t STALE_MS      = 60000;  // no good reading this long -> "--"
constexpr int PINGS_PER_READ     = 5;      // median of this many pings
constexpr uint32_t PING_GAP_MS   = 60;     // let echoes die out between pings

struct Tote {
  const char *name;
  int trig, echo;
  float cm;        // last good sensor-to-water distance, NAN if none
  uint32_t lastGoodMs;
};

Tote totes[3] = {
  // name  trig echo
  {"T1",   2,   3,   NAN, 0},
  {"T2",  20,  23,   NAN, 0},
  {"T3",  16,  17,   NAN, 0},
};

// ---------- Colours (RGB565) ----------
constexpr uint16_t C_OK     = 0x2E6E;  // green
constexpr uint16_t C_WARN   = 0xF524;  // amber
constexpr uint16_t C_ALARM  = 0xE267;  // red
constexpr uint16_t C_NODATA = 0x34DB;  // blue
constexpr uint16_t C_DIM    = 0x8C51;  // grey
constexpr uint16_t C_BLACK  = 0x0000;
constexpr uint16_t C_WHITE  = 0xFFFF;

Adafruit_ST7789 tft(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
GFXcanvas16 *canvas = nullptr;   // full-frame buffer so the screen doesn't flicker

// ---------- Sensors ----------
float pingOnceCm(const Tote &t) {
  digitalWrite(t.trig, LOW);
  delayMicroseconds(2);
  digitalWrite(t.trig, HIGH);
  delayMicroseconds(10);
  digitalWrite(t.trig, LOW);
  unsigned long us = pulseIn(t.echo, HIGH, 30000UL);   // ~5 m max
  if (us == 0) return NAN;
  float cm = us * 0.0343f / 2.0f;
  if (cm < 2.0f || cm > 400.0f) return NAN;
  return cm;
}

float readMedianCm(const Tote &t) {
  float v[PINGS_PER_READ];
  int n = 0;
  for (int i = 0; i < PINGS_PER_READ; i++) {
    float cm = pingOnceCm(t);
    if (!isnan(cm)) v[n++] = cm;
    delay(PING_GAP_MS);
  }
  if (n < 3) return NAN;              // too many misses, don't trust it
  for (int i = 1; i < n; i++) {       // insertion sort
    float x = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
    v[j + 1] = x;
  }
  return v[n / 2];
}

// Water depth in inches from the last good distance
float depthIn(const Tote &t) {
  if (isnan(t.cm)) return NAN;
  float d = SENSOR_HEIGHT_IN - t.cm / CM_PER_IN;
  return d < 0.0f ? 0.0f : d;
}

// Depth (in) -> gallons, straight line between calibration points
float gallonsFromDepth(float d) {
  if (isnan(d)) return NAN;
  if (d <= CAL_DEPTH_IN[0]) return 0.0f;
  for (int i = 1; i < CAL_N; i++) {
    if (d <= CAL_DEPTH_IN[i]) {
      float f = (d - CAL_DEPTH_IN[i - 1]) / (CAL_DEPTH_IN[i] - CAL_DEPTH_IN[i - 1]);
      return CAL_GAL[i - 1] + f * (CAL_GAL[i] - CAL_GAL[i - 1]);
    }
  }
  // Above the table: keep the last slope
  int j = CAL_N - 1;
  float slope = (CAL_GAL[j] - CAL_GAL[j - 1]) / (CAL_DEPTH_IN[j] - CAL_DEPTH_IN[j - 1]);
  float g = CAL_GAL[j] + (d - CAL_DEPTH_IN[j]) * slope;
  return g > TOTE_MAX_GAL ? TOTE_MAX_GAL : g;
}

float gallons(const Tote &t) {
  return gallonsFromDepth(depthIn(t));
}

// 0 = no data, 1 = OK, 2 = watch, 3 = low
int status(float g) {
  if (isnan(g)) return 0;
  if (g < LOW_GAL) return 3;
  if (g < LOW_GAL + WATCH_MARGIN) return 2;
  return 1;
}

uint16_t statusColour(int s) {
  switch (s) {
    case 1:  return C_OK;
    case 2:  return C_WARN;
    case 3:  return C_ALARM;
    default: return C_NODATA;
  }
}

// ---------- Display ----------
void printCentered(Adafruit_GFX &g, const char *txt, int cx, int y, uint8_t size, uint16_t col) {
  int16_t x1, y1; uint16_t w, h;
  g.setTextSize(size);
  g.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  g.setTextColor(col);
  g.setCursor(cx - (int)w / 2, y);
  g.print(txt);
}

void drawScreen(Adafruit_GFX &g) {
  g.fillScreen(C_BLACK);
  for (int i = 0; i < 3; i++) {
    int x = i * 80, cx = x + 40;
    float gal = gallons(totes[i]);
    uint16_t sc = statusColour(status(gal));

    // Status strip with tote name
    g.fillRect(x + 3, 0, 74, 30, sc);
    printCentered(g, totes[i].name, cx, 8, 2, C_BLACK);

    // Vertical fill gauge (full = 20 gal)
    const int gx = x + 18, gy = 40, gw = 44, gh = 150;
    g.drawRect(gx, gy, gw, gh, C_DIM);
    if (!isnan(gal)) {
      float frac = gal / GAUGE_FULL_GAL;
      if (frac > 1.0f) frac = 1.0f;
      int h = (int)((gh - 4) * frac);
      if (h > 0) g.fillRect(gx + 2, gy + gh - 2 - h, gw - 4, h, sc);
    }

    // Gallons
    char buf[8];
    if (isnan(gal)) strcpy(buf, "--");
    else snprintf(buf, sizeof(buf), "%.0f", gal);
    printCentered(g, buf, cx, 204, 3, C_WHITE);
  }
}

void refreshDisplay() {
  if (canvas) {
    drawScreen(*canvas);
    tft.drawRGBBitmap(0, 0, canvas->getBuffer(), 240, 240);
  } else {
    drawScreen(tft);   // fallback if the frame buffer couldn't be allocated
  }
}

// ---------- RGB LED: worst tote ----------
void updateLed() {
  int worst = 0;
  bool anyMissing = false;
  for (auto &t : totes) {
    int s = status(gallons(t));
    if (s == 0) anyMissing = true;
    worst = max(worst, s);
  }
  if (worst == 1 && anyMissing) worst = 0;   // don't show green while a tote has no data
  const uint8_t B = 40;                      // brightness
  switch (worst) {
    case 1:  rgbLedWrite(PIN_RGB, 0, B, 0);            break;  // green
    case 2:  rgbLedWrite(PIN_RGB, B, B * 45 / 100, 0); break;  // amber
    case 3:  rgbLedWrite(PIN_RGB, B, 0, 0);            break;  // red
    default: rgbLedWrite(PIN_RGB, 0, 0, B);            break;  // blue
  }
}

// ---------- Main ----------
void setup() {
  Serial.begin(115200);

  for (auto &t : totes) {
    pinMode(t.trig, OUTPUT);
    digitalWrite(t.trig, LOW);
    pinMode(t.echo, INPUT);
  }

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  SPI.begin(PIN_LCD_SCLK, -1, PIN_LCD_MOSI, PIN_LCD_CS);
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(2);

  canvas = new GFXcanvas16(240, 240);
  if (!canvas->getBuffer()) { delete canvas; canvas = nullptr; }

  rgbLedWrite(PIN_RGB, 0, 0, 40);
  refreshDisplay();
  Serial.println("Tote monitor started");
}

void loop() {
  static uint32_t lastRead = 0;
  uint32_t now = millis();
  if (lastRead != 0 && now - lastRead < READ_EVERY_MS) { delay(10); return; }
  lastRead = now;

  for (auto &t : totes) {
    float cm = readMedianCm(t);           // sensors are read one at a time
    if (!isnan(cm)) { t.cm = cm; t.lastGoodMs = millis(); }
    else if (t.lastGoodMs && millis() - t.lastGoodMs > STALE_MS) t.cm = NAN;

    float d = depthIn(t);
    float g = gallons(t);
    Serial.printf("%s  distance: %5.2f in   depth: %5.2f in   volume: %5.1f gal\n",
                  t.name,
                  isnan(cm) ? -1.0f : cm / CM_PER_IN,
                  isnan(d) ? -1.0f : d,
                  isnan(g) ? -1.0f : g);
  }
  Serial.println();

  refreshDisplay();
  updateLed();
}
