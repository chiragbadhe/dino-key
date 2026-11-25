/*
  DINO KEY - touch keychain with a face
  ESP32-C3 SuperMini + 1.3" SH1106 OLED (I2C) + TTP223 touch sensor
  + 350 mAh LiPo + TP4056 charger + slide switch

  Modes:  HOME -> CLOCK -> STOPWATCH -> DINO -> FACES -> BATTERY -> HOME ...

  Touch controls
    Tap              -> next mode
    Double tap       -> action:  HOME/FACES = next face
                                 STOPWATCH  = start / stop
                                 DINO       = start game
    Hold (0.6 s)     -> HOME/FACES = pet it (hearts!)   STOPWATCH = reset
    Hold 3 s         -> sleep  (touch again to wake)
    In DINO game     -> every touch = jump
  Auto-sleep after 3 minutes with no touch (not while stopwatch runs).
  Ignore it for 1 minute on HOME and it gets sad.

  Library needed: "U8g2" by olikraus (Arduino Library Manager)
  Board: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
*/

#include <Arduino.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <time.h>
#include "esp_sleep.h"

// ------------------------- USER SETTINGS -------------------------
const char* WIFI_SSID = "C601";      // used once at power-on to get the time
const char* WIFI_PASS = "112233445566";
const char* TZ_INFO   = "IST-5:30";          // India. Change for other time zones.

#define HAS_BAT_SENSE 0   // 1 = you wired the 2-resistor divider to PIN_BAT, 0 = no resistors
#define PIN_SDA    6
#define PIN_SCL    7
#define PIN_TOUCH  3      // TTP223 OUT (HIGH when touched)
#define PIN_BAT    1      // battery + through two equal resistors (10k-470k)
#define PIN_USB    -1     // optional: charger 5V through two equal resistors to GPIO 4 (-1 = not used)
#define BAT_DIVIDER 2.0   // (R1+R2)/R2 - tweak if your % reads wrong

const uint32_t AUTO_SLEEP_MS = 3UL * 60UL * 1000UL;
const uint32_t LONELY_MS     = 60UL * 1000UL;
// -----------------------------------------------------------------

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

enum Mode { M_HOME, M_CLOCK, M_STOPWATCH, M_DINO, M_FACES, M_BATTERY, MODE_COUNT };

// all custom types live up here so the Arduino IDE can see them early
enum Gesture { G_NONE, G_PRESS, G_TAP, G_DOUBLE, G_HOLD_START, G_HOLD_END, G_SLEEP };
struct Cactus { float x; int w, h; };
const char* MODE_NAMES[] = { "HOME", "CLOCK", "STOPWATCH", "DINO RUN", "FACES", "BATTERY" };

// kept across deep sleep
RTC_DATA_ATTR int      mode = M_HOME;
RTC_DATA_ATTR int      faceIdx = 1;
RTC_DATA_ATTR uint32_t dinoBest = 0;
RTC_DATA_ATTR uint32_t petCount = 0;

uint32_t lastActivity = 0, lastBatRead = 0, modeBannerUntil = 0, happyUntil = 0;

// ------------------------- FACES -------------------------
enum Eye   { E_OPEN, E_CLOSED, E_HAPPY, E_SAD, E_ANGRY, E_HEART, E_X, E_ROUND, E_SHADES, E_SLEEPY };
enum Mouth { MO_NONE, MO_SMILE, MO_FROWN, MO_FLAT, MO_O, MO_GRIN };

struct Face { const char* name; uint8_t eyeL, eyeR, mouth; };
const Face FACES[] = {
  { "Neutral",   E_OPEN,   E_OPEN,   MO_FLAT  },
  { "Happy",     E_HAPPY,  E_HAPPY,  MO_SMILE },
  { "Excited",   E_ROUND,  E_ROUND,  MO_GRIN  },
  { "Wink",      E_OPEN,   E_CLOSED, MO_SMILE },
  { "Laugh",     E_HAPPY,  E_HAPPY,  MO_GRIN  },
  { "Sleepy",    E_SLEEPY, E_SLEEPY, MO_FLAT  },
  { "Surprised", E_ROUND,  E_ROUND,  MO_O     },
  { "Sad",       E_SAD,    E_SAD,    MO_FROWN },
  { "Angry",     E_ANGRY,  E_ANGRY,  MO_FROWN },
  { "Love",      E_HEART,  E_HEART,  MO_SMILE },
  { "Cool",      E_SHADES, E_SHADES, MO_SMILE },
  { "Error",     E_X,      E_X,      MO_FLAT  },
  { "Sleeping",  E_CLOSED, E_CLOSED, MO_NONE  },
};
const int FACE_COUNT = sizeof(FACES) / sizeof(FACES[0]);
const int FACE_LAUGH = 4, FACE_SLEEPY = 5, FACE_SAD = 7, FACE_LOVE = 9, FACE_SLEEPING = FACE_COUNT - 1;

bool     blinking = false;
uint32_t nextBlinkAt = 0, blinkEndAt = 0;

void drawEye(uint8_t type, int cx, int cy, int ew, int eh, int side /* -1 left, +1 right */) {
  int left = cx - ew / 2, top = cy - eh / 2;
  int innerX = cx - side * ew / 2, outerX = cx + side * ew / 2;
  if (blinking && (type == E_OPEN || type == E_ROUND || type == E_SAD || type == E_ANGRY)) type = E_CLOSED;

  switch (type) {
    case E_OPEN:
      oled.drawRBox(left, top, ew, eh, max(1, min(ew, eh) / 4));
      break;
    case E_CLOSED:
      oled.drawBox(left, cy, ew, 3);
      break;
    case E_HAPPY:   // ^ ^
      for (int i = 0; i < 3; i++) {
        oled.drawLine(left, cy + eh / 4 + i, cx, cy - eh / 4 + i);
        oled.drawLine(cx, cy - eh / 4 + i, left + ew, cy + eh / 4 + i);
      }
      break;
    case E_SAD:
    case E_ANGRY:
      oled.drawRBox(left, top, ew, eh, max(1, min(ew, eh) / 4));
      oled.setDrawColor(0);
      if (type == E_SAD) oled.drawTriangle(innerX, top - 1, outerX + side, top - 1, outerX + side, top + eh / 2);
      else               oled.drawTriangle(outerX, top - 1, innerX - side, top - 1, innerX - side, top + eh / 2);
      oled.setDrawColor(1);
      break;
    case E_SLEEPY:
      oled.drawRBox(left, top, ew, eh, max(1, min(ew, eh) / 4));
      oled.setDrawColor(0);
      oled.drawBox(left - 1, top - 1, ew + 2, eh / 2 + 1);
      oled.setDrawColor(1);
      break;
    case E_HEART: {
      int r = max(2, ew / 4);
      oled.drawDisc(cx - r, cy - r / 2, r);
      oled.drawDisc(cx + r, cy - r / 2, r);
      oled.drawTriangle(cx - 2 * r - 1, cy - r / 2 + 1, cx + 2 * r + 1, cy - r / 2 + 1, cx, cy + eh / 2);
      break;
    }
    case E_X:
      for (int d = -1; d <= 1; d++) {
        oled.drawLine(left + d, top, left + ew + d, top + eh);
        oled.drawLine(left + ew + d, top, left + d, top + eh);
      }
      break;
    case E_ROUND: {
      int r = min(ew, eh) / 2;
      oled.drawDisc(cx, cy, r);
      oled.setDrawColor(0);
      oled.drawDisc(cx + r / 3, cy - r / 3, max(1, r / 3));   // shine
      oled.setDrawColor(1);
      break;
    }
    case E_SHADES:
      oled.drawBox(left - 2, cy - eh / 4, ew + 4, eh / 2 + 2);
      break;
  }
}

void drawMouth(uint8_t type, int cx, int cy, int mw) {
  int rx = max(2, mw / 2), ry = max(2, mw / 4);
  switch (type) {
    case MO_SMILE:
      oled.drawEllipse(cx, cy, rx, ry, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      oled.drawEllipse(cx, cy + 1, rx, ry, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    case MO_FROWN:
      oled.drawEllipse(cx, cy + ry, rx, ry, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      oled.drawEllipse(cx, cy + ry + 1, rx, ry, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    case MO_FLAT:
      oled.drawBox(cx - mw / 3, cy + 1, 2 * mw / 3, 2);
      break;
    case MO_O:
      oled.drawFilledEllipse(cx, cy + 2, max(2, mw / 5), ry + 1);
      break;
    case MO_GRIN:
      oled.drawFilledEllipse(cx, cy, rx, ry + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
  }
}

void drawSmallHeart(int x, int y) {
  oled.drawDisc(x - 2, y, 2);
  oled.drawDisc(x + 2, y, 2);
  oled.drawTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5);
}

// draws a face inside the box (ox, oy, W, H)
void drawFace(int idx, int ox, int oy, int W, int H, bool blush) {
  const Face& f = FACES[idx];
  int ew = W * 20 / 100, eh = H * 40 / 100;
  int ey = oy + H * 38 / 100;
  int exL = ox + W / 2 - W * 22 / 100, exR = ox + W / 2 + W * 22 / 100;
  drawEye(f.eyeL, exL, ey, ew, eh, -1);
  drawEye(f.eyeR, exR, ey, ew, eh, +1);
  if (f.eyeL == E_SHADES) oled.drawBox(exL + ew / 2, ey - eh / 4, exR - exL - ew, 2);  // glasses bridge
  drawMouth(f.mouth, ox + W / 2, oy + H * 72 / 100, W * 18 / 100);
  if (blush) {                                                         // ///  ///
    int by = ey + eh / 2 + 3;
    for (int i = 0; i < 3; i++) {
      oled.drawLine(exL - ew / 2 - 4 + i * 4, by + 3, exL - ew / 2 - 2 + i * 4, by);
      oled.drawLine(exR + ew / 2 - 6 + i * 4, by + 3, exR + ew / 2 - 4 + i * 4, by);
    }
  }
  if (idx == FACE_SLEEPING) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(ox + W - 22, oy + 12, "z");
    oled.drawStr(ox + W - 14, oy + 8, "Z");
  }
}

void updateBlink() {
  uint32_t t = millis();
  if (!blinking && t >= nextBlinkAt) { blinking = true; blinkEndAt = t + 140; }
  if (blinking && t >= blinkEndAt) { blinking = false; nextBlinkAt = t + random(2500, 6000); }
}

// ------------------------- BATTERY -------------------------
float batV = 0; int batPct = 0; bool usbIn = false;

int voltsToPercent(float v) {
  const float tbl[][2] = { {4.20,100},{4.10,90},{4.00,80},{3.90,65},{3.80,50},
                           {3.70,35},{3.60,20},{3.50,10},{3.40,5},{3.30,0} };
  if (v >= tbl[0][0]) return 100;
  for (int i = 1; i < 10; i++)
    if (v >= tbl[i][0])
      return tbl[i][1] + (v - tbl[i][0]) * (tbl[i-1][1] - tbl[i][1]) / (tbl[i-1][0] - tbl[i][0]);
  return 0;
}

void readBattery() {
  usbIn = false;
#if PIN_USB >= 0
  usbIn = digitalRead(PIN_USB);
#endif
  if (!HAS_BAT_SENSE) { batPct = -1; return; }   // no sensor wired
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT);
  float v = (sum / 16.0f) * BAT_DIVIDER / 1000.0f;
  batV = (batV == 0) ? v : batV * 0.8f + v * 0.2f;   // smooth
  batPct = voltsToPercent(batV);
}

void drawBatteryIcon(int x, int y, int w, int h, int pct) {
  oled.drawFrame(x, y, w, h);
  oled.drawBox(x + w, y + h / 4, 2, h / 2);
  int fill = (w - 4) * pct / 100;
  if (fill > 0) oled.drawBox(x + 2, y + 2, fill, h - 4);
}

// ------------------------- TIME -------------------------
bool timeValid() { time_t n = time(nullptr); return n > 1700000000; }

void syncTimeWiFi() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(10, 30, "Syncing time...");
  oled.sendBuffer();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    t0 = millis();
    while (!timeValid() && millis() - t0 < 8000) delay(200);
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);          // save battery
}

// ------------------------- TOUCH -------------------------
bool touchHeld = false;          // true while a hold (> 0.6 s) is going on

Gesture readTouch() {
  static bool last = false, holdFired = false, sleepFired = false;
  static uint32_t downAt = 0, lastUp = 0;
  static uint8_t taps = 0;
  bool now = digitalRead(PIN_TOUCH);
  uint32_t t = millis();
  Gesture g = G_NONE;

  if (now && !last) { downAt = t; holdFired = sleepFired = false; g = G_PRESS; }
  else if (now && !holdFired && t - downAt > 600) { holdFired = true; taps = 0; touchHeld = true; g = G_HOLD_START; }
  else if (now && holdFired && !sleepFired && t - downAt > 3000) { sleepFired = true; g = G_SLEEP; }
  else if (!now && last) {
    if (holdFired) { touchHeld = false; g = G_HOLD_END; }
    else if (t - downAt > 30) { taps++; lastUp = t; }
  }
  else if (taps && !now && t - lastUp > 300) { g = (taps >= 2) ? G_DOUBLE : G_TAP; taps = 0; }
  last = now;
  return g;
}

// ------------------------- SLEEP -------------------------
void goToSleep() {
  oled.clearBuffer();
  drawFace(FACE_SLEEPING, 0, 0, 128, 64, false);
  oled.sendBuffer();
  delay(1000);
  while (digitalRead(PIN_TOUCH)) delay(10);     // wait for finger release
  oled.setPowerSave(1);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_TOUCH, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// ------------------------- PET IT -------------------------
// which face to show right now, based on mood
int moodFace(bool allowLonely) {
  uint32_t t = millis();
  if (touchHeld) return FACE_LOVE;                               // being petted
  if (t < happyUntil) return FACE_LAUGH;                         // just petted
  if (usbIn) return FACE_LAUGH;                                  // charging
  if (batPct >= 0 && batPct < 15) return FACE_SLEEPY;            // low battery
  if (allowLonely && t - lastActivity > LONELY_MS) return FACE_SAD;  // ignored
  return faceIdx;
}

void drawPetHearts(int ox, int oy, int W, int H) {
  if (!touchHeld) return;
  uint32_t t = millis();
  const int xs[3] = { ox + 8, ox + W - 9, ox + W - 24 };
  for (int i = 0; i < 3; i++) {
    int phase = (t / 25 + i * 17) % H;
    int y = oy + H - 6 - phase;
    if (y > oy + 2) drawSmallHeart(xs[i], y);
  }
}

// ------------------------- STOPWATCH -------------------------
bool swRunning = false;
uint32_t swStart = 0, swElapsed = 0;

uint32_t swTime() { return swElapsed + (swRunning ? millis() - swStart : 0); }
void swToggle() {
  if (swRunning) { swElapsed += millis() - swStart; swRunning = false; }
  else           { swStart = millis(); swRunning = true; }
}
void swReset() { swRunning = false; swElapsed = 0; }

// ------------------------- DINO RUN -------------------------
const int GROUND_Y = 56, DINO_X = 10, DINO_W = 11, DINO_H = 11;
bool dinoPlaying = false;
uint32_t dinoScore = 0, dinoOverAt = 0, dinoLastFrame = 0;
float dinoY = 0, dinoVy = 0, dinoDist = 0, groundShift = 0;
Cactus cacti[2];

float dinoSpeed() { return min(160.0f, 60.0f + dinoScore * 0.15f); }

void placeCactus(int i, float afterX) {
  cacti[i].x = afterX + dinoSpeed() * 0.6f + random(25, 75);
  cacti[i].w = random(4, 8);
  cacti[i].h = random(8, 15);
}

void dinoStart() {
  dinoPlaying = true; dinoScore = 0; dinoDist = 0;
  dinoY = 0; dinoVy = 0;
  cacti[0].x = 0; placeCactus(0, 100);
  placeCactus(1, cacti[0].x);
  dinoLastFrame = millis();
}

void dinoJump() { if (dinoY == 0) dinoVy = 230; }

void dinoUpdate() {
  uint32_t t = millis();
  float dt = (t - dinoLastFrame) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  dinoLastFrame = t;

  // jump physics (dinoY = height above ground)
  if (dinoY > 0 || dinoVy > 0) {
    dinoVy -= 900 * dt;
    dinoY += dinoVy * dt;
    if (dinoY <= 0) { dinoY = 0; dinoVy = 0; }
  }
  float sp = dinoSpeed();
  dinoDist += sp * dt;
  dinoScore = dinoDist / 5;
  groundShift += sp * dt;

  for (int i = 0; i < 2; i++) {
    cacti[i].x -= sp * dt;
    if (cacti[i].x + cacti[i].w < 0) placeCactus(i, max(cacti[0].x, cacti[1].x));
    // collision (shrink dino box by 2 px to be fair)
    int dTop = GROUND_Y - DINO_H - (int)dinoY + 2, dBot = GROUND_Y - (int)dinoY;
    int cTop = GROUND_Y - cacti[i].h;
    if (DINO_X + 2 < cacti[i].x + cacti[i].w && DINO_X + DINO_W - 2 > cacti[i].x && dBot > cTop && dTop < GROUND_Y) {
      dinoPlaying = false; dinoOverAt = t;
      if (dinoScore > dinoBest) dinoBest = dinoScore;
    }
  }
}

void drawDino(int x, int y, bool running) {        // y = top
  oled.drawBox(x + 5, y, 6, 4);                    // head
  oled.setDrawColor(0); oled.drawPixel(x + 9, y + 1); oled.setDrawColor(1);  // eye
  oled.drawBox(x + 2, y + 3, 6, 6);                // body
  oled.drawBox(x, y + 4, 2, 2);                    // tail
  oled.drawPixel(x + 8, y + 5);                    // arm
  bool step = running && ((millis() / 100) % 2);
  oled.drawBox(x + 3, y + 9, 2, step ? 1 : 2);     // legs
  oled.drawBox(x + 6, y + 9, 2, step ? 2 : 1);
}

void drawCactus(const Cactus& c) {
  int x = (int)c.x;
  oled.drawBox(x, GROUND_Y - c.h, c.w, c.h);
  if (c.h > 10) {                                  // little arms
    oled.drawBox(x - 2, GROUND_Y - c.h + 4, 2, 3);
    oled.drawBox(x + c.w, GROUND_Y - c.h + 2, 2, 3);
  }
}

void screenDino() {
  char buf[24];
  oled.drawHLine(0, GROUND_Y, 128);
  for (int gx = -((int)groundShift % 16); gx < 128; gx += 16) oled.drawPixel(gx + 5, GROUND_Y + 3);

  oled.setFont(u8g2_font_5x7_tf);
  snprintf(buf, sizeof buf, "HI %05lu  %05lu", (unsigned long)dinoBest, (unsigned long)dinoScore);
  oled.drawStr(128 - oled.getStrWidth(buf) - 1, 7, buf);

  if (dinoPlaying) {
    drawDino(DINO_X, GROUND_Y - DINO_H - (int)dinoY, dinoY == 0);
    for (int i = 0; i < 2; i++) if (cacti[i].x < 128) drawCactus(cacti[i]);
  } else {
    drawDino(DINO_X, GROUND_Y - DINO_H, false);
    oled.setFont(u8g2_font_7x14B_tf);
    const char* title = dinoOverAt ? "GAME OVER" : "DINO RUN";
    oled.drawStr((128 - oled.getStrWidth(title)) / 2, 28, title);
    oled.setFont(u8g2_font_5x7_tf);
    const char* hint = "2x tap: play";
    oled.drawStr((128 - oled.getStrWidth(hint)) / 2, 42, hint);
  }
}

// ------------------------- SCREENS -------------------------
void screenHome() {
  // top bar: time + date (+ battery), then divider line
  oled.setFont(u8g2_font_6x10_tf);
  char buf[24];
  if (timeValid()) { time_t n = time(nullptr); strftime(buf, sizeof buf, "%I:%M %p", localtime(&n)); }
  else strcpy(buf, "--:--");
  oled.drawStr(2, 9, buf);

  int rightEdge = 126;
  if (batPct >= 0) { drawBatteryIcon(106, 2, 18, 8, batPct); rightEdge = 102; }
  if (usbIn) { oled.drawStr(rightEdge - 6, 9, "+"); rightEdge -= 8; }
  if (timeValid()) {
    time_t n = time(nullptr);
    strftime(buf, sizeof buf, "%a %d", localtime(&n));   // e.g. "Wed 25"
    oled.drawStr(rightEdge - oled.getStrWidth(buf), 9, buf);
  }
  oled.drawHLine(0, 12, 128);                         // divider

  drawFace(moodFace(true), 0, 14, 128, 50, touchHeld);
  drawPetHearts(0, 14, 128, 50);
}

void screenClock() {
  if (!timeValid()) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(8, 28, "Time not set");
    oled.drawStr(8, 44, "Check WiFi in code");
    return;
  }
  time_t n = time(nullptr); struct tm* tm = localtime(&n);
  char hm[8], ap[4], date[20];
  strftime(hm, sizeof hm, "%I:%M", tm);
  strftime(ap, sizeof ap, "%p", tm);
  strftime(date, sizeof date, "%a %d %b", tm);
  oled.setFont(u8g2_font_logisoso32_tn);
  oled.drawStr(4, 40, hm);
  oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(106, 40, ap);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(4, 58, date);
  if ((n % 2) == 0) oled.drawDisc(120, 54, 2);       // seconds tick
}

void screenStopwatch() {
  uint32_t ms = swTime();
  unsigned long mins = (ms / 60000) % 100, secs = (ms / 1000) % 60, cs = (ms / 10) % 100;
  char buf[12];

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(2, 9, "STOPWATCH");
  const char* st = swRunning ? "RUN" : (ms ? "PAUSE" : "READY");
  oled.drawStr(126 - oled.getStrWidth(st), 9, st);
  oled.drawHLine(0, 12, 128);

  snprintf(buf, sizeof buf, "%02lu:%02lu", mins, secs);
  oled.setFont(u8g2_font_logisoso32_tn);
  oled.drawStr(4, 50, buf);
  snprintf(buf, sizeof buf, ".%02lu", cs);
  oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(102, 50, buf);

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(2, 63, "2x:start/stop hold:reset");
}

void screenBattery() {
  if (batPct < 0) {
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(10, 28, "No battery sensor");
    oled.drawStr(10, 44, "(add 2 resistors)");
    return;
  }
  drawBatteryIcon(20, 10, 82, 30, batPct);
  char buf[24];
  oled.setFont(u8g2_font_7x14B_tf);
  snprintf(buf, sizeof buf, "%d%%  %.2fV", batPct, batV);
  oled.drawStr(20, 56, buf);
  if (usbIn) { oled.setFont(u8g2_font_6x10_tf); oled.drawStr(36, 30, "CHARGING"); }
}

void screenFaces() {
  drawFace(moodFace(false), 0, 0, 128, 64, touchHeld);
  drawPetHearts(0, 0, 128, 64);
}

// ------------------------- MAIN -------------------------
void bootSplash() {
  for (int f = 0; f < 3; f++) {                     // dino runs in, then title
    oled.clearBuffer();
    oled.drawHLine(0, GROUND_Y, 128);
    drawDino(10 + f * 4, GROUND_Y - DINO_H, f < 2);
    oled.setFont(u8g2_font_logisoso16_tr);
    const char* name = "DINO KEY";
    oled.drawStr((128 - oled.getStrWidth(name)) / 2, 30, name);
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(78, 54, "v1.0");
    oled.sendBuffer();
    delay(500);
  }
}

void setup() {
  pinMode(PIN_TOUCH, INPUT);
#if PIN_USB >= 0
  pinMode(PIN_USB, INPUT);
#endif
  analogReadResolution(12);
  oled.begin();
  oled.setContrast(180);
  randomSeed(esp_random());

  setenv("TZ", TZ_INFO, 1); tzset();               // keep time zone after deep sleep
  bool coldBoot = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED;
  if (coldBoot) bootSplash();                      // name screen on power-on
  if (coldBoot || !timeValid()) syncTimeWiFi();

  uint32_t t0 = millis();
  while (digitalRead(PIN_TOUCH) && millis() - t0 < 2000) delay(10);  // ignore wake touch
  readBattery();
  lastActivity = millis();
}

void handleGesture(Gesture g, uint32_t t) {
  if (g == G_NONE) return;
  if (g == G_SLEEP) { goToSleep(); return; }

  // Dino: while playing, touch = jump and nothing else
  if (mode == M_DINO && dinoPlaying) { if (g == G_PRESS) dinoJump(); return; }
  if (mode == M_DINO && t - dinoOverAt < 800) return;   // ignore taps right after crash

  switch (g) {
    case G_TAP:
      mode = (mode + 1) % MODE_COUNT;
      modeBannerUntil = t + 700;
      break;

    case G_DOUBLE:
      if (mode == M_HOME || mode == M_FACES) faceIdx = (faceIdx + 1) % (FACE_COUNT - 1);
      else if (mode == M_STOPWATCH) swToggle();
      else if (mode == M_DINO) dinoStart();
      else { blinking = true; blinkEndAt = t + 140; }
      break;

    case G_HOLD_START:
      if (mode == M_HOME || mode == M_FACES) petCount++;
      else if (mode == M_STOPWATCH) swReset();
      break;

    case G_HOLD_END:
      if (mode == M_HOME || mode == M_FACES) happyUntil = t + 3000;   // happy after petting
      break;

    default: break;
  }
}

void loop() {
  uint32_t t = millis();
  Gesture g = readTouch();
  if (g != G_NONE) lastActivity = t;
  handleGesture(g, t);

  bool busy = swRunning || dinoPlaying || usbIn;
  if (!busy && t - lastActivity > AUTO_SLEEP_MS) goToSleep();
  if (t - lastBatRead > 2000) { readBattery(); lastBatRead = t; }
  updateBlink();
  if (mode == M_DINO && dinoPlaying) dinoUpdate();

  oled.clearBuffer();
  if (t < modeBannerUntil) {
    oled.setFont(u8g2_font_7x14B_tf);
    int w = oled.getStrWidth(MODE_NAMES[mode]);
    oled.drawStr((128 - w) / 2, 38, MODE_NAMES[mode]);
  } else {
    switch (mode) {
      case M_HOME:      screenHome();      break;
      case M_CLOCK:     screenClock();     break;
      case M_STOPWATCH: screenStopwatch(); break;
      case M_DINO:      screenDino();      break;
      case M_FACES:     screenFaces();     break;
      case M_BATTERY:   screenBattery();   break;
    }
  }
  oled.sendBuffer();
  delay(10);
}
