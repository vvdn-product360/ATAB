// ============================================================
//  AIRPORT TRAVEL BAND — ESP32 + GC9A01 1.28" Round Display
//  v3.0 — Clean compile, no struct conflicts
//
//  WIRING:
//    GC9A01 MOSI → GPIO 23
//    GC9A01 SCLK → GPIO 18
//    GC9A01 CS   → GPIO  5
//    GC9A01 DC   → GPIO  2
//    GC9A01 RST  → GPIO  4
//    GC9A01 VCC  → 3.3V
//    GC9A01 GND  → GND
//
//    BTN PREV   → GPIO 12  (other leg to GND)
//    BTN NEXT   → GPIO 13  (other leg to GND)
//    BTN SELECT → GPIO 14  (other leg to GND)
//
//  LIBRARIES NEEDED (Arduino Library Manager):
//    ArduinoJson  by Benoit Blanchon  (v6.x)
//    WiFi + HTTPClient — built-in with ESP32 board package
//
//  BEFORE FLASHING: update WIFI_SSID, WIFI_PASS, SERVER_BASE
// ============================================================

#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================================
//  USER CONFIG — EDIT THESE
// ============================================================
const char* WIFI_SSID   = "Obito";            // your WiFi name
const char* WIFI_PASS   = "abikowsi";        // your WiFi password
const char* SERVER_BASE = "http://10.164.243.121:3000"; // your PC LAN IP:port
const char* DEVICE_ID   = "ATAB_BAND_001";

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define PIN_MOSI   23
#define PIN_SCLK   18
#define PIN_CS      5
#define PIN_DC      2
#define PIN_RST     4

#define BTN_PREV   12
#define BTN_NEXT   13
#define BTN_SELECT 14

// ============================================================
//  RGB565 COLOR HELPERS
//  Note: macro named COL() to avoid any ESP32 core conflicts
// ============================================================
#define COL(r,g,b) ( ((uint16_t)((r) & 0xF8) << 8) | ((uint16_t)((g) & 0xFC) << 3) | ((uint16_t)(b) >> 3) )

static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_BG     = COL(  8, 14, 30);   // deep navy
static const uint16_t C_PANEL  = COL( 15, 30, 60);   // panel bg
static const uint16_t C_ACCENT = COL(  0,180,220);   // cyan
static const uint16_t C_YELLOW = COL(255,220,  0);
static const uint16_t C_GREEN  = COL(  0,220, 80);
static const uint16_t C_RED    = COL(255, 60, 60);
static const uint16_t C_ORANGE = COL(255,150,  0);
static const uint16_t C_GRAY   = COL(100,110,130);
static const uint16_t C_DKGRAY = COL( 22, 32, 52);
static const uint16_t C_PLANE  = COL(180,200,255);

// ============================================================
//  ALERT STRUCT & STORAGE
// ============================================================
#define MAX_ALERTS 10

typedef struct {
  int  id;
  char flight[12];
  char destination[24];
  char flightTime[8];    // renamed from 'time' — avoids C++ stdlib clash
  char gate[8];
  char status[20];
} FlightAlert;

FlightAlert alerts[MAX_ALERTS];
int  alertCount    = 0;
int  selectedIndex = 0;
int  lastServerId  = -1;
bool detailView    = false;
bool needsRedraw   = true;

// ============================================================
//  DISPLAY — LOW LEVEL SPI
// ============================================================
static inline void dispCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}

static inline void dispData(uint8_t d) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH);
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  dispCmd(0x2A);
  dispData(x0 >> 8); dispData(x0 & 0xFF);
  dispData(x1 >> 8); dispData(x1 & 0xFF);
  dispCmd(0x2B);
  dispData(y0 >> 8); dispData(y0 & 0xFF);
  dispData(y1 >> 8); dispData(y1 & 0xFF);
  dispCmd(0x2C);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x >= 240 || y >= 240 || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 240) w = 240 - x;
  if (y + h > 240) h = 240 - y;
  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)(x+w-1), (uint16_t)(y+h-1));
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)(c & 0xFF);
  for (int32_t i = 0; i < (int32_t)w * h; i++) {
    SPI.transfer(hi);
    SPI.transfer(lo);
  }
  digitalWrite(PIN_CS, HIGH);
}

void fillScreen(uint16_t c) { fillRect(0, 0, 240, 240, c); }

void drawPixel(int16_t x, int16_t y, uint16_t c) {
  if (x < 0 || x >= 240 || y < 0 || y >= 240) return;
  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)x, (uint16_t)y);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer((uint8_t)(c >> 8));
  SPI.transfer((uint8_t)(c & 0xFF));
  digitalWrite(PIN_CS, HIGH);
}

void drawHLine(int16_t x, int16_t y, int16_t len, uint16_t c) {
  fillRect(x, y, len, 1, c);
}
void drawVLine(int16_t x, int16_t y, int16_t len, uint16_t c) {
  fillRect(x, y, 1, len, c);
}

void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t x = r, y = 0, err = 0;
  while (x >= y) {
    drawPixel(cx+x,cy+y,c); drawPixel(cx-x,cy+y,c);
    drawPixel(cx+x,cy-y,c); drawPixel(cx-x,cy-y,c);
    drawPixel(cx+y,cy+x,c); drawPixel(cx-y,cy+x,c);
    drawPixel(cx+y,cy-x,c); drawPixel(cx-y,cy-x,c);
    y++;
    if (err <= 0) { err += 2*y + 1; }
    if (err >  0) { x--; err -= 2*x + 1; }
  }
}

void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t dx = (int16_t)sqrtf((float)(r*r - dy*dy));
    fillRect(cx - dx, cy + dy, 2*dx + 1, 1, c);
  }
}

// ============================================================
//  5×7 PIXEL FONT  (ASCII 32–126)
// ============================================================
static const uint8_t FONT5X7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
  {0x00,0x00,0x5F,0x00,0x00}, // 33 !
  {0x00,0x07,0x00,0x07,0x00}, // 34 "
  {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
  {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
  {0x23,0x13,0x08,0x64,0x62}, // 37 %
  {0x36,0x49,0x55,0x22,0x50}, // 38 &
  {0x00,0x05,0x03,0x00,0x00}, // 39 '
  {0x00,0x1C,0x22,0x41,0x00}, // 40 (
  {0x00,0x41,0x22,0x1C,0x00}, // 41 )
  {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
  {0x08,0x08,0x3E,0x08,0x08}, // 43 +
  {0x00,0x50,0x30,0x00,0x00}, // 44 ,
  {0x08,0x08,0x08,0x08,0x08}, // 45 -
  {0x00,0x60,0x60,0x00,0x00}, // 46 .
  {0x20,0x10,0x08,0x04,0x02}, // 47 /
  {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
  {0x00,0x42,0x7F,0x40,0x00}, // 49 1
  {0x42,0x61,0x51,0x49,0x46}, // 50 2
  {0x21,0x41,0x45,0x4B,0x31}, // 51 3
  {0x18,0x14,0x12,0x7F,0x10}, // 52 4
  {0x27,0x45,0x45,0x45,0x39}, // 53 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
  {0x01,0x71,0x09,0x05,0x03}, // 55 7
  {0x36,0x49,0x49,0x49,0x36}, // 56 8
  {0x06,0x49,0x49,0x29,0x1E}, // 57 9
  {0x00,0x36,0x36,0x00,0x00}, // 58 :
  {0x00,0x56,0x36,0x00,0x00}, // 59 ;
  {0x00,0x08,0x14,0x22,0x41}, // 60 <
  {0x14,0x14,0x14,0x14,0x14}, // 61 =
  {0x41,0x22,0x14,0x08,0x00}, // 62 >
  {0x02,0x01,0x51,0x09,0x06}, // 63 ?
  {0x32,0x49,0x79,0x41,0x3E}, // 64 @
  {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
  {0x7F,0x49,0x49,0x49,0x36}, // 66 B
  {0x3E,0x41,0x41,0x41,0x22}, // 67 C
  {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
  {0x7F,0x49,0x49,0x49,0x41}, // 69 E
  {0x7F,0x09,0x09,0x09,0x01}, // 70 F
  {0x3E,0x41,0x49,0x49,0x7A}, // 71 G
  {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
  {0x00,0x41,0x7F,0x41,0x00}, // 73 I
  {0x20,0x40,0x41,0x3F,0x01}, // 74 J
  {0x7F,0x08,0x14,0x22,0x41}, // 75 K
  {0x7F,0x40,0x40,0x40,0x40}, // 76 L
  {0x7F,0x02,0x0C,0x02,0x7F}, // 77 M
  {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
  {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
  {0x7F,0x09,0x09,0x09,0x06}, // 80 P
  {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
  {0x7F,0x09,0x19,0x29,0x46}, // 82 R
  {0x46,0x49,0x49,0x49,0x31}, // 83 S
  {0x01,0x01,0x7F,0x01,0x01}, // 84 T
  {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
  {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
  {0x3F,0x40,0x38,0x40,0x3F}, // 87 W
  {0x63,0x14,0x08,0x14,0x63}, // 88 X
  {0x07,0x08,0x70,0x08,0x07}, // 89 Y
  {0x61,0x51,0x49,0x45,0x43}, // 90 Z
  {0x00,0x7F,0x41,0x41,0x00}, // 91 [
  {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
  {0x00,0x41,0x41,0x7F,0x00}, // 93 ]
  {0x04,0x02,0x01,0x02,0x04}, // 94 ^
  {0x40,0x40,0x40,0x40,0x40}, // 95 _
  {0x00,0x01,0x02,0x04,0x00}, // 96 `
  {0x20,0x54,0x54,0x54,0x78}, // 97 a
  {0x7F,0x48,0x44,0x44,0x38}, // 98 b
  {0x38,0x44,0x44,0x44,0x20}, // 99 c
  {0x38,0x44,0x44,0x48,0x7F}, // 100 d
  {0x38,0x54,0x54,0x54,0x18}, // 101 e
  {0x08,0x7E,0x09,0x01,0x02}, // 102 f
  {0x08,0x54,0x54,0x54,0x3C}, // 103 g
  {0x7F,0x08,0x04,0x04,0x78}, // 104 h
  {0x00,0x44,0x7D,0x40,0x00}, // 105 i
  {0x20,0x40,0x44,0x3D,0x00}, // 106 j
  {0x7F,0x10,0x28,0x44,0x00}, // 107 k
  {0x00,0x41,0x7F,0x40,0x00}, // 108 l
  {0x7C,0x04,0x18,0x04,0x78}, // 109 m
  {0x7C,0x08,0x04,0x04,0x78}, // 110 n
  {0x38,0x44,0x44,0x44,0x38}, // 111 o
  {0x7C,0x14,0x14,0x14,0x08}, // 112 p
  {0x08,0x14,0x14,0x18,0x7C}, // 113 q
  {0x7C,0x08,0x04,0x04,0x08}, // 114 r
  {0x48,0x54,0x54,0x54,0x20}, // 115 s
  {0x04,0x3F,0x44,0x40,0x20}, // 116 t
  {0x3C,0x40,0x40,0x40,0x7C}, // 117 u
  {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
  {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
  {0x44,0x28,0x10,0x28,0x44}, // 120 x
  {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
  {0x44,0x64,0x54,0x4C,0x44}, // 122 z
  {0x00,0x08,0x36,0x41,0x00}, // 123 {
  {0x00,0x00,0x7F,0x00,0x00}, // 124 |
  {0x00,0x41,0x36,0x08,0x00}, // 125 }
  {0x08,0x04,0x08,0x10,0x08}, // 126 ~
};

// Draw one character at (x,y). scale=1→5×7px, scale=2→10×14px
void drawChar(int16_t x, int16_t y, char ch, uint16_t color, uint8_t scale) {
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t* glyph = FONT5X7[(uint8_t)ch - 32];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&glyph[col]);
    for (uint8_t row = 0; row < 7; row++) {
      if (bits & (1 << row)) {
        fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

// Draw null-terminated string, returns x after last char
int16_t drawStr(int16_t x, int16_t y, const char* s, uint16_t color, uint8_t scale) {
  while (*s) {
    drawChar(x, y, *s++, color, scale);
    x += 6 * scale;
  }
  return x;
}

// Draw string horizontally centered on cx
void drawStrC(int16_t cx, int16_t y, const char* s, uint16_t color, uint8_t scale) {
  int16_t w = (int16_t)(strlen(s) * 6 * scale);
  drawStr(cx - w / 2, y, s, color, scale);
}

// Copy src to dst, truncating at maxChars and appending ".." if needed
void safeStr(const char* src, char* dst, int maxChars) {
  int len = (int)strlen(src);
  if (len <= maxChars) {
    strcpy(dst, src);
  } else {
    strncpy(dst, src, maxChars - 2);
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
  }
}

// ============================================================
//  GC9A01 INIT SEQUENCE
// ============================================================
void initDisplay() {
  digitalWrite(PIN_RST, LOW);  delay(120);
  digitalWrite(PIN_RST, HIGH); delay(120);

  dispCmd(0xEF);
  dispCmd(0xEB); dispData(0x14);
  dispCmd(0xFE);
  dispCmd(0xEF);
  dispCmd(0xEB); dispData(0x14);
  dispCmd(0x84); dispData(0x40);
  dispCmd(0x85); dispData(0xFF);
  dispCmd(0x86); dispData(0xFF);
  dispCmd(0x87); dispData(0xFF);
  dispCmd(0x88); dispData(0x0A);
  dispCmd(0x89); dispData(0x21);
  dispCmd(0x8A); dispData(0x00);
  dispCmd(0x8B); dispData(0x80);
  dispCmd(0x8C); dispData(0x01);
  dispCmd(0x8D); dispData(0x01);
  dispCmd(0x8E); dispData(0xFF);
  dispCmd(0x8F); dispData(0xFF);
  dispCmd(0xB6); dispData(0x00); dispData(0x20);
  dispCmd(0x36); dispData(0xC8);  // MX+MY+BGR — correct for Waveshare 1.28"
  dispCmd(0x3A); dispData(0x05);  // RGB565
  dispCmd(0x90); dispData(0x08); dispData(0x08); dispData(0x08); dispData(0x08);
  dispCmd(0xBD); dispData(0x06);
  dispCmd(0xBC); dispData(0x00);
  dispCmd(0xFF); dispData(0x60); dispData(0x01); dispData(0x04);
  dispCmd(0xC3); dispData(0x13);
  dispCmd(0xC4); dispData(0x13);
  dispCmd(0xC9); dispData(0x22);
  dispCmd(0xBE); dispData(0x11);
  dispCmd(0xE1); dispData(0x10); dispData(0x0E);
  dispCmd(0xDF); dispData(0x21); dispData(0x0C); dispData(0x02);
  dispCmd(0xF0); dispData(0x45);dispData(0x09);dispData(0x08);dispData(0x08);dispData(0x26);dispData(0x2A);
  dispCmd(0xF1); dispData(0x43);dispData(0x70);dispData(0x72);dispData(0x36);dispData(0x37);dispData(0x6F);
  dispCmd(0xF2); dispData(0x45);dispData(0x09);dispData(0x08);dispData(0x08);dispData(0x26);dispData(0x2A);
  dispCmd(0xF3); dispData(0x43);dispData(0x70);dispData(0x72);dispData(0x36);dispData(0x37);dispData(0x6F);
  dispCmd(0xED); dispData(0x1B); dispData(0x0B);
  dispCmd(0xAE); dispData(0x77);
  dispCmd(0xCD); dispData(0x63);
  dispCmd(0x70); dispData(0x07);dispData(0x07);dispData(0x04);dispData(0x0E);dispData(0x0F);dispData(0x09);dispData(0x07);dispData(0x08);dispData(0x03);
  dispCmd(0xE8); dispData(0x34);
  dispCmd(0x62); dispData(0x18);dispData(0x0D);dispData(0x71);dispData(0xED);dispData(0x70);dispData(0x70);
                 dispData(0x18);dispData(0x0F);dispData(0x71);dispData(0xEF);dispData(0x70);dispData(0x70);
  dispCmd(0x63); dispData(0x18);dispData(0x11);dispData(0x71);dispData(0xF1);dispData(0x70);dispData(0x70);
                 dispData(0x18);dispData(0x13);dispData(0x71);dispData(0xF3);dispData(0x70);dispData(0x70);
  dispCmd(0x64); dispData(0x28);dispData(0x29);dispData(0xF1);dispData(0x01);dispData(0xF1);dispData(0x00);dispData(0x07);
  dispCmd(0x66); dispData(0x3C);dispData(0x00);dispData(0xCD);dispData(0x67);dispData(0x45);dispData(0x45);
                 dispData(0x10);dispData(0x00);dispData(0x00);dispData(0x00);
  dispCmd(0x67); dispData(0x00);dispData(0x3C);dispData(0x00);dispData(0x00);dispData(0x00);dispData(0x01);
                 dispData(0x54);dispData(0x10);dispData(0x32);dispData(0x98);
  dispCmd(0x74); dispData(0x10);dispData(0x85);dispData(0x80);dispData(0x00);dispData(0x00);dispData(0x4E);dispData(0x00);
  dispCmd(0x98); dispData(0x3E); dispData(0x07);
  dispCmd(0x11); delay(120);
  dispCmd(0x29);
}

// ============================================================
//  STATUS COLOR
// ============================================================
uint16_t statusColor(const char* s) {
  String st(s);
  st.toLowerCase();
  if (st.indexOf("boarding")    >= 0) return C_GREEN;
  if (st.indexOf("delayed")     >= 0) return C_RED;
  if (st.indexOf("gate change") >= 0) return C_YELLOW;
  if (st.indexOf("final")       >= 0) return C_ORANGE;
  return C_ACCENT;
}

// ============================================================
//  SMALL DECORATIVE WIDGETS
// ============================================================
void drawWifiDot(int16_t x, int16_t y, bool connected) {
  uint16_t c = connected ? C_GREEN : C_GRAY;
  drawCircle(x, y, 7, c);
  drawCircle(x, y, 4, c);
  fillCircle(x, y, 2, c);
}

void drawPlaneTiny(int16_t x, int16_t y, uint16_t c) {
  fillRect(x+3, y+3, 9, 3, c);   // body
  fillRect(x+11,y+4, 4, 1, c);   // nose
  fillRect(x+4, y+1, 5, 1, c);   // wing top
  fillRect(x+4, y+6, 5, 1, c);   // wing bot
  fillRect(x+2, y+2, 2, 1, c);   // tail top
  fillRect(x+2, y+6, 2, 1, c);   // tail bot
}

// ============================================================
//  SCREEN: SPLASH
// ============================================================
void drawSplash() {
  fillScreen(C_BG);
  // Brand rings
  for (int r = 115; r >= 110; r--) drawCircle(120, 120, r, C_ACCENT);
  for (int r = 105; r >= 103; r--) drawCircle(120, 120, r, C_DKGRAY);
  // Plane body
  fillRect(68,  116, 100, 8,  C_PLANE);
  fillRect(166, 117,   8, 6,  C_PLANE);
  fillRect(173, 119,   4, 2,  C_PLANE);
  // Wings
  fillRect(92,  94,  50, 5,  C_PLANE);
  fillRect(78,  98,  14, 22, C_PLANE);
  // Tail
  fillRect(70,  106, 18, 5,  C_PLANE);
  fillRect(70,  127, 18, 5,  C_PLANE);
  // Text
  drawStrC(120, 155, "TRAVEL BAND", C_ACCENT, 2);
  drawStrC(120, 178, "ATAB-001",    C_GRAY,   2);
  drawStrC(120, 200, "Connecting..",C_GRAY,   1);
}

// ============================================================
//  SCREEN: NO WIFI
// ============================================================
void drawNoWifi() {
  fillScreen(C_BG);
  drawCircle(120, 120, 115, C_DKGRAY);
  drawStrC(120,  75, "NO WIFI",      C_RED,    2);
  drawStrC(120, 105, "Check SSID",   C_GRAY,   1);
  drawStrC(120, 120, "& Password",   C_GRAY,   1);
  drawStrC(120, 148, WIFI_SSID,      C_YELLOW, 1);
  drawStrC(120, 165, "Retrying...",  C_GRAY,   1);
}

// ============================================================
//  SCREEN: LIST VIEW  (up to 5 rows)
// ============================================================
void drawListView() {
  fillScreen(C_BG);

  // ── Top bar ──────────────────────────────────────────────
  fillRect(0, 0, 240, 24, C_PANEL);
  drawStrC(110, 7, "FLIGHT ALERTS", C_ACCENT, 1);
  drawWifiDot(225, 12, WiFi.status() == WL_CONNECTED);
  drawPlaneTiny(6, 5, C_ACCENT);

  // ── Bottom nav bar ───────────────────────────────────────
  fillRect(0, 216, 240, 24, C_PANEL);
  drawStr(  8, 222, "<PREV",  C_GRAY, 1);
  drawStrC(120, 222, "SELECT", C_GRAY, 1);
  drawStr(187, 222, "NEXT>",  C_GRAY, 1);

  // ── No data ───────────────────────────────────────────────
  if (alertCount == 0) {
    drawStrC(120,  98, "No alerts yet.",      C_GRAY, 1);
    drawStrC(120, 114, "Waiting for server.", C_GRAY, 1);
    return;
  }

  // ── Rows ─────────────────────────────────────────────────
  const int ROW_START = 28;
  const int ROW_H     = 37;
  const int MAX_VIS   = 5;

  int scrollOff = 0;
  if (selectedIndex >= MAX_VIS) scrollOff = selectedIndex - MAX_VIS + 1;

  for (int i = 0; i < MAX_VIS; i++) {
    int idx = i + scrollOff;
    if (idx >= alertCount) break;

    FlightAlert& a   = alerts[idx];
    int16_t      y   = ROW_START + i * ROW_H;
    bool         sel = (idx == selectedIndex);

    if (sel) {
      fillRect(6, y, 228, ROW_H - 3, C_DKGRAY);
      fillRect(6, y,   3, ROW_H - 3, C_ACCENT);  // left accent bar
    }

    // Flight (large)
    char fl[12]; safeStr(a.flight, fl, 7);
    drawStr(14, y + 3, fl, sel ? C_WHITE : C_ACCENT, 2);

    // Destination
    char ds[16]; safeStr(a.destination, ds, 9);
    drawStr(82, y + 3, ds, sel ? C_WHITE : C_GRAY, 1);

    // Time
    drawStr(82, y + 16, a.flightTime, C_YELLOW, 1);

    // Gate
    char gl[10]; snprintf(gl, sizeof(gl), "G:%s", a.gate);
    drawStr(148, y + 16, gl, C_ACCENT, 1);

    // Status dot
    fillCircle(220, y + 12, 5, statusColor(a.status));

    // Divider (non-selected rows only)
    if (!sel) drawHLine(6, y + ROW_H - 4, 228, C_DKGRAY);
  }

  // ── Page indicator dots ───────────────────────────────────
  if (alertCount > 1) {
    int n    = min(alertCount, 10);
    int dotX = 120 - (n * 9) / 2;
    for (int i = 0; i < n; i++) {
      if (i == selectedIndex)
        fillCircle(dotX + i * 9, 211, 3, C_ACCENT);
      else
        fillCircle(dotX + i * 9, 211, 2, C_DKGRAY);
    }
  }
}

// ============================================================
//  SCREEN: DETAIL VIEW  (one alert, full screen)
// ============================================================
void drawDetailView() {
  fillScreen(C_BG);
  FlightAlert& a = alerts[selectedIndex];

  // ── Top bar with flight number ────────────────────────────
  fillRect(0, 0, 240, 30, C_PANEL);
  char hdr[20]; snprintf(hdr, sizeof(hdr), ">> %s <<", a.flight);
  drawStrC(120, 8, hdr, C_ACCENT, 2);

  // Outer ring
  drawCircle(120, 120, 116, C_DKGRAY);

  // Status color stripe under top bar
  uint16_t sc = statusColor(a.status);
  fillRect(0, 32, 240, 4, sc);

  // ── Destination ───────────────────────────────────────────
  drawStrC(120, 46, "DESTINATION", C_GRAY,  1);
  char ds[20]; safeStr(a.destination, ds, 13);
  drawStrC(120, 60, ds, C_WHITE, 2);

  drawHLine(18, 88, 204, C_DKGRAY);

  // ── Time | Gate ───────────────────────────────────────────
  drawStrC( 68, 95, "TIME", C_GRAY, 1);
  drawStrC(172, 95, "GATE", C_GRAY, 1);
  drawVLine(120, 91, 55, C_DKGRAY);
  drawStrC( 68, 110, a.flightTime, C_YELLOW, 2);
  drawStrC(172, 110, a.gate,       C_ACCENT, 2);

  drawHLine(18, 148, 204, C_DKGRAY);

  // ── Status ────────────────────────────────────────────────
  drawStrC(120, 155, "STATUS", C_GRAY, 1);
  char st[20]; safeStr(a.status, st, 13);
  drawStrC(120, 170, st, sc, 2);

  // ── Bottom nav ────────────────────────────────────────────
  fillRect(0, 206, 240, 34, C_PANEL);
  drawStr(10, 215, "<BACK", C_GRAY, 1);

  char pg[12]; snprintf(pg, sizeof(pg), "%d/%d", selectedIndex+1, alertCount);
  drawStrC(120, 215, pg, C_DKGRAY, 1);

  drawStr(182, 215, "P/N>", C_GRAY, 1);
}

// ============================================================
//  FETCH ALERTS FROM SERVER
// ============================================================
void fetchAlerts() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected, skipping fetch.");
    return;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/devices/%s/notifications", SERVER_BASE, DEVICE_ID);
  Serial.printf("[HTTP] GET %s\n", url);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);   // 5-second timeout
  int code = http.GET();
  Serial.printf("[HTTP] Response: %d\n", code);

  if (code == 200) {
    String payload = http.getString();
    Serial.printf("[JSON] Payload length: %d bytes\n", payload.length());

    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[JSON] Parse error: %s\n", err.c_str());
      http.end();
      return;
    }

    JsonArray arr   = doc["notifications"].as<JsonArray>();
    int       count = (int)arr.size();
    Serial.printf("[JSON] Notifications in response: %d\n", count);

    if (count == 0) { http.end(); return; }

    int newestId = arr[count - 1]["id"] | 0;

    if (newestId == lastServerId && alertCount > 0) {
      Serial.println("[Fetch] No new alerts.");
      http.end();
      return;
    }

    lastServerId = newestId;
    alertCount   = 0;

    // Store newest-first (iterate array backwards)
    for (int i = count - 1; i >= 0 && alertCount < MAX_ALERTS; i--) {
      JsonObject obj = arr[i].as<JsonObject>();
      FlightAlert& out = alerts[alertCount];
      out.id = obj["id"] | 0;

      const char* msg = obj["message"] | "";
      char buf[128];
      strncpy(buf, msg, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      // Parse "FLIGHT | DEST | TIME | GATE | STATUS"
      char* parts[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
      int   pi    = 0;
      char* token = strtok(buf, "|");
      while (token && pi < 5) {
        while (*token == ' ') token++;           // ltrim
        int tlen = (int)strlen(token);
        while (tlen > 0 && token[tlen-1] == ' ') token[--tlen] = '\0'; // rtrim
        parts[pi++] = token;
        token = strtok(nullptr, "|");
      }

      strncpy(out.flight,      parts[0] ? parts[0] : "----",  sizeof(out.flight)      - 1);
      strncpy(out.destination, parts[1] ? parts[1] : "---",   sizeof(out.destination) - 1);
      strncpy(out.flightTime,  parts[2] ? parts[2] : "--:--", sizeof(out.flightTime)  - 1);
      strncpy(out.gate,        parts[3] ? parts[3] : "--",    sizeof(out.gate)        - 1);
      strncpy(out.status,      parts[4] ? parts[4] : "---",   sizeof(out.status)      - 1);

      // Null-terminate all fields defensively
      out.flight[sizeof(out.flight)-1]           = '\0';
      out.destination[sizeof(out.destination)-1] = '\0';
      out.flightTime[sizeof(out.flightTime)-1]   = '\0';
      out.gate[sizeof(out.gate)-1]               = '\0';
      out.status[sizeof(out.status)-1]           = '\0';

      Serial.printf("[Alert %d] %s | %s | %s | %s | %s\n",
        out.id, out.flight, out.destination,
        out.flightTime, out.gate, out.status);

      alertCount++;
    }

    selectedIndex = 0;
    needsRedraw   = true;
    Serial.printf("[Fetch] Stored %d alerts. Newest ID=%d\n", alertCount, newestId);

  } else {
    Serial.printf("[HTTP] Error code: %d\n", code);
  }

  http.end();
}

// ============================================================
//  BUTTON STATE  (plain globals — zero conflict risk)
// ============================================================
static bool     gPrevLast = HIGH,  gNextLast = HIGH,  gSelLast = HIGH;
static uint32_t gPrevTime = 0,     gNextTime = 0,     gSelTime = 0;

bool btnFell(uint8_t pin, bool& lastState, uint32_t& lastTime) {
  bool cur = (bool)digitalRead(pin);
  if (cur == LOW && lastState == HIGH && (millis() - lastTime) > 50UL) {
    lastState = LOW;
    lastTime  = millis();
    return true;
  }
  if (cur == HIGH) lastState = HIGH;
  return false;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n==============================");
  Serial.println(" ATAB Band Firmware Starting");
  Serial.println("==============================");

  // Display pins
  pinMode(PIN_CS,  OUTPUT); digitalWrite(PIN_CS,  HIGH);
  pinMode(PIN_DC,  OUTPUT); digitalWrite(PIN_DC,  HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);

  // SPI + display init
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  initDisplay();
  Serial.println("[Display] GC9A01 initialized.");

  // Button pins
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  Serial.println("[Buttons] GPIO 12=PREV  13=NEXT  14=SELECT");

  // Splash screen while WiFi connects
  drawSplash();

  // WiFi
  Serial.printf("[WiFi] Connecting to: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 24) {
    delay(500);
    tries++;
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Server: %s\n", SERVER_BASE);
    fetchAlerts();
  } else {
    Serial.println("[WiFi] FAILED. Check SSID and password.");
    drawNoWifi();
    delay(3000);
  }

  needsRedraw = true;
}

// ============================================================
//  LOOP
// ============================================================
static unsigned long gLastFetch = 0;
static const unsigned long FETCH_MS = 8000UL;   // poll every 8 seconds

void loop() {
  // ── Periodic server poll ──────────────────────────────────
  if (millis() - gLastFetch >= FETCH_MS) {
    gLastFetch = millis();
    fetchAlerts();
  }

  // ── Button handling ───────────────────────────────────────
  bool redraw = false;

  if (btnFell(BTN_PREV, gPrevLast, gPrevTime)) {
    Serial.println("[BTN] PREV pressed");
    if (detailView) {
      // In detail view: go to older alert
      if (selectedIndex < alertCount - 1) { selectedIndex++; redraw = true; }
    } else {
      // In list view: scroll up to older
      if (selectedIndex < alertCount - 1) { selectedIndex++; redraw = true; }
    }
  }

  if (btnFell(BTN_NEXT, gNextLast, gNextTime)) {
    Serial.println("[BTN] NEXT pressed");
    if (detailView) {
      // In detail view: go to newer alert
      if (selectedIndex > 0) { selectedIndex--; redraw = true; }
    } else {
      // In list view: scroll down to newer
      if (selectedIndex > 0) { selectedIndex--; redraw = true; }
    }
  }

  if (btnFell(BTN_SELECT, gSelLast, gSelTime)) {
    Serial.println("[BTN] SELECT pressed");
    if (alertCount > 0) {
      detailView = !detailView;
      Serial.printf("[UI] Switched to %s view\n", detailView ? "DETAIL" : "LIST");
      redraw = true;
    }
  }

  // ── Redraw if needed ──────────────────────────────────────
  if (redraw || needsRedraw) {
    needsRedraw = false;
    if (detailView && alertCount > 0) {
      Serial.printf("[Draw] Detail view, alert %d/%d\n", selectedIndex+1, alertCount);
      drawDetailView();
    } else {
      Serial.printf("[Draw] List view, %d alerts, selected=%d\n", alertCount, selectedIndex);
      drawListView();
    }
  }

  delay(20);
}// ============================================================
//  AIRPORT TRAVEL BAND — ESP32 + GC9A01 1.28" Round Display
//  v3.1 — Added vibration motor + DFMiniPlayer audio alert
//
//  WIRING:
//    GC9A01 MOSI → GPIO 23
//    GC9A01 SCLK → GPIO 18
//    GC9A01 CS   → GPIO  5
//    GC9A01 DC   → GPIO  2
//    GC9A01 RST  → GPIO  4
//    GC9A01 VCC  → 3.3V
//    GC9A01 GND  → GND
//
//    BTN PREV   → GPIO 12  (other leg to GND)
//    BTN NEXT   → GPIO 13  (other leg to GND)
//    BTN SELECT → GPIO 14  (other leg to GND)
//
//    VIBRATION MOTOR → GPIO 27  (via transistor/driver to GND)
//
//    DFMINI PLAYER:
//      ESP32 TX2 (GPIO 17) → DFPlayer RX (via 1kΩ resistor)
//      DFPlayer TX         → ESP32 RX2 (GPIO 16)
//      DFPlayer VCC        → 5V
//      DFPlayer GND        → GND
//      DFPlayer SPK_1/2    → Speaker
//
//  LIBRARIES NEEDED (Arduino Library Manager):
//    ArduinoJson          by Benoit Blanchon  (v6.x)
//    DFRobotDFPlayerMini  by DFRobot
//    WiFi + HTTPClient — built-in with ESP32 board package
//
//  BEFORE FLASHING: update WIFI_SSID, WIFI_PASS, SERVER_BASE
// ============================================================

#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ============================================================
//  USER CONFIG — EDIT THESE
// ============================================================
const char* WIFI_SSID   = "Obito";
const char* WIFI_PASS   = "abikowsi";
const char* SERVER_BASE = "http://10.164.243.121:3000";
const char* DEVICE_ID   = "ATAB_BAND_001";

// ============================================================
//  PIN DEFINITIONS
// ============================================================
#define PIN_MOSI   23
#define PIN_SCLK   18
#define PIN_CS      5
#define PIN_DC      2
#define PIN_RST     4

#define BTN_PREV   12
#define BTN_NEXT   13
#define BTN_SELECT 14

// ── NEW: Vibration motor pin ──────────────────────────────
#define PIN_VIBRO  27

// ── NEW: DFMiniPlayer UART pins ───────────────────────────
#define DF_RX_PIN  16   // ESP32 RX2 ← DFPlayer TX
#define DF_TX_PIN  17   // ESP32 TX2 → DFPlayer RX

// ============================================================
//  RGB565 COLOR HELPERS
// ============================================================
#define COL(r,g,b) ( ((uint16_t)((r) & 0xF8) << 8) | ((uint16_t)((g) & 0xFC) << 3) | ((uint16_t)(b) >> 3) )

static const uint16_t C_BLACK  = 0x0000;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_BG     = COL(  8, 14, 30);
static const uint16_t C_PANEL  = COL( 15, 30, 60);
static const uint16_t C_ACCENT = COL(  0,180,220);
static const uint16_t C_YELLOW = COL(255,220,  0);
static const uint16_t C_GREEN  = COL(  0,220, 80);
static const uint16_t C_RED    = COL(255, 60, 60);
static const uint16_t C_ORANGE = COL(255,150,  0);
static const uint16_t C_GRAY   = COL(100,110,130);
static const uint16_t C_DKGRAY = COL( 22, 32, 52);
static const uint16_t C_PLANE  = COL(180,200,255);

// ============================================================
//  ALERT STRUCT & STORAGE
// ============================================================
#define MAX_ALERTS 10

typedef struct {
  int  id;
  char flight[12];
  char destination[24];
  char flightTime[8];
  char gate[8];
  char status[20];
} FlightAlert;

FlightAlert alerts[MAX_ALERTS];
int  alertCount    = 0;
int  selectedIndex = 0;
int  lastServerId  = -1;
bool detailView    = false;
bool needsRedraw   = true;

// ── NEW: DFMiniPlayer object on UART2 ─────────────────────
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini dfPlayer;

// ── NEW: Trigger alert feedback (vibro + audio) ───────────
void triggerAlertFeedback() {
  // Vibrate for 600 ms
  digitalWrite(PIN_VIBRO, HIGH);
  delay(600);
  digitalWrite(PIN_VIBRO, LOW);

  // Play track 1 (0001.mp3) on DFMiniPlayer
  dfPlayer.play(1);
  Serial.println("[Alert] Vibration + audio triggered.");
}

// ============================================================
//  DISPLAY — LOW LEVEL SPI
// ============================================================
static inline void dispCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}

static inline void dispData(uint8_t d) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH);
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  dispCmd(0x2A);
  dispData(x0 >> 8); dispData(x0 & 0xFF);
  dispData(x1 >> 8); dispData(x1 & 0xFF);
  dispCmd(0x2B);
  dispData(y0 >> 8); dispData(y0 & 0xFF);
  dispData(y1 >> 8); dispData(y1 & 0xFF);
  dispCmd(0x2C);
}

void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
  if (x >= 240 || y >= 240 || w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 240) w = 240 - x;
  if (y + h > 240) h = 240 - y;
  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)(x+w-1), (uint16_t)(y+h-1));
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  uint8_t hi = (uint8_t)(c >> 8), lo = (uint8_t)(c & 0xFF);
  for (int32_t i = 0; i < (int32_t)w * h; i++) {
    SPI.transfer(hi);
    SPI.transfer(lo);
  }
  digitalWrite(PIN_CS, HIGH);
}

void fillScreen(uint16_t c) { fillRect(0, 0, 240, 240, c); }

void drawPixel(int16_t x, int16_t y, uint16_t c) {
  if (x < 0 || x >= 240 || y < 0 || y >= 240) return;
  setWindow((uint16_t)x, (uint16_t)y, (uint16_t)x, (uint16_t)y);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer((uint8_t)(c >> 8));
  SPI.transfer((uint8_t)(c & 0xFF));
  digitalWrite(PIN_CS, HIGH);
}

void drawHLine(int16_t x, int16_t y, int16_t len, uint16_t c) {
  fillRect(x, y, len, 1, c);
}
void drawVLine(int16_t x, int16_t y, int16_t len, uint16_t c) {
  fillRect(x, y, 1, len, c);
}

void drawCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  int16_t x = r, y = 0, err = 0;
  while (x >= y) {
    drawPixel(cx+x,cy+y,c); drawPixel(cx-x,cy+y,c);
    drawPixel(cx+x,cy-y,c); drawPixel(cx-x,cy-y,c);
    drawPixel(cx+y,cy+x,c); drawPixel(cx-y,cy+x,c);
    drawPixel(cx+y,cy-x,c); drawPixel(cx-y,cy-x,c);
    y++;
    if (err <= 0) { err += 2*y + 1; }
    if (err >  0) { x--; err -= 2*x + 1; }
  }
}

void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t c) {
  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t dx = (int16_t)sqrtf((float)(r*r - dy*dy));
    fillRect(cx - dx, cy + dy, 2*dx + 1, 1, c);
  }
}

// ============================================================
//  5×7 PIXEL FONT  (ASCII 32–126)
// ============================================================
static const uint8_t FONT5X7[][5] PROGMEM = {
  {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
  {0x00,0x00,0x5F,0x00,0x00}, // 33 !
  {0x00,0x07,0x00,0x07,0x00}, // 34 "
  {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
  {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
  {0x23,0x13,0x08,0x64,0x62}, // 37 %
  {0x36,0x49,0x55,0x22,0x50}, // 38 &
  {0x00,0x05,0x03,0x00,0x00}, // 39 '
  {0x00,0x1C,0x22,0x41,0x00}, // 40 (
  {0x00,0x41,0x22,0x1C,0x00}, // 41 )
  {0x08,0x2A,0x1C,0x2A,0x08}, // 42 *
  {0x08,0x08,0x3E,0x08,0x08}, // 43 +
  {0x00,0x50,0x30,0x00,0x00}, // 44 ,
  {0x08,0x08,0x08,0x08,0x08}, // 45 -
  {0x00,0x60,0x60,0x00,0x00}, // 46 .
  {0x20,0x10,0x08,0x04,0x02}, // 47 /
  {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
  {0x00,0x42,0x7F,0x40,0x00}, // 49 1
  {0x42,0x61,0x51,0x49,0x46}, // 50 2
  {0x21,0x41,0x45,0x4B,0x31}, // 51 3
  {0x18,0x14,0x12,0x7F,0x10}, // 52 4
  {0x27,0x45,0x45,0x45,0x39}, // 53 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
  {0x01,0x71,0x09,0x05,0x03}, // 55 7
  {0x36,0x49,0x49,0x49,0x36}, // 56 8
  {0x06,0x49,0x49,0x29,0x1E}, // 57 9
  {0x00,0x36,0x36,0x00,0x00}, // 58 :
  {0x00,0x56,0x36,0x00,0x00}, // 59 ;
  {0x00,0x08,0x14,0x22,0x41}, // 60 
  {0x14,0x14,0x14,0x14,0x14}, // 61 =
  {0x41,0x22,0x14,0x08,0x00}, // 62 >
  {0x02,0x01,0x51,0x09,0x06}, // 63 ?
  {0x32,0x49,0x79,0x41,0x3E}, // 64 @
  {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
  {0x7F,0x49,0x49,0x49,0x36}, // 66 B
  {0x3E,0x41,0x41,0x41,0x22}, // 67 C
  {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
  {0x7F,0x49,0x49,0x49,0x41}, // 69 E
  {0x7F,0x09,0x09,0x09,0x01}, // 70 F
  {0x3E,0x41,0x49,0x49,0x7A}, // 71 G
  {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
  {0x00,0x41,0x7F,0x41,0x00}, // 73 I
  {0x20,0x40,0x41,0x3F,0x01}, // 74 J
  {0x7F,0x08,0x14,0x22,0x41}, // 75 K
  {0x7F,0x40,0x40,0x40,0x40}, // 76 L
  {0x7F,0x02,0x0C,0x02,0x7F}, // 77 M
  {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
  {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
  {0x7F,0x09,0x09,0x09,0x06}, // 80 P
  {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
  {0x7F,0x09,0x19,0x29,0x46}, // 82 R
  {0x46,0x49,0x49,0x49,0x31}, // 83 S
  {0x01,0x01,0x7F,0x01,0x01}, // 84 T
  {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
  {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
  {0x3F,0x40,0x38,0x40,0x3F}, // 87 W
  {0x63,0x14,0x08,0x14,0x63}, // 88 X
  {0x07,0x08,0x70,0x08,0x07}, // 89 Y
  {0x61,0x51,0x49,0x45,0x43}, // 90 Z
  {0x00,0x7F,0x41,0x41,0x00}, // 91 [
  {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
  {0x00,0x41,0x41,0x7F,0x00}, // 93 ]
  {0x04,0x02,0x01,0x02,0x04}, // 94 ^
  {0x40,0x40,0x40,0x40,0x40}, // 95 _
  {0x00,0x01,0x02,0x04,0x00}, // 96 `
  {0x20,0x54,0x54,0x54,0x78}, // 97 a
  {0x7F,0x48,0x44,0x44,0x38}, // 98 b
  {0x38,0x44,0x44,0x44,0x20}, // 99 c
  {0x38,0x44,0x44,0x48,0x7F}, // 100 d
  {0x38,0x54,0x54,0x54,0x18}, // 101 e
  {0x08,0x7E,0x09,0x01,0x02}, // 102 f
  {0x08,0x54,0x54,0x54,0x3C}, // 103 g
  {0x7F,0x08,0x04,0x04,0x78}, // 104 h
  {0x00,0x44,0x7D,0x40,0x00}, // 105 i
  {0x20,0x40,0x44,0x3D,0x00}, // 106 j
  {0x7F,0x10,0x28,0x44,0x00}, // 107 k
  {0x00,0x41,0x7F,0x40,0x00}, // 108 l
  {0x7C,0x04,0x18,0x04,0x78}, // 109 m
  {0x7C,0x08,0x04,0x04,0x78}, // 110 n
  {0x38,0x44,0x44,0x44,0x38}, // 111 o
  {0x7C,0x14,0x14,0x14,0x08}, // 112 p
  {0x08,0x14,0x14,0x18,0x7C}, // 113 q
  {0x7C,0x08,0x04,0x04,0x08}, // 114 r
  {0x48,0x54,0x54,0x54,0x20}, // 115 s
  {0x04,0x3F,0x44,0x40,0x20}, // 116 t
  {0x3C,0x40,0x40,0x40,0x7C}, // 117 u
  {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
  {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
  {0x44,0x28,0x10,0x28,0x44}, // 120 x
  {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
  {0x44,0x64,0x54,0x4C,0x44}, // 122 z
  {0x00,0x08,0x36,0x41,0x00}, // 123 {
  {0x00,0x00,0x7F,0x00,0x00}, // 124 |
  {0x00,0x41,0x36,0x08,0x00}, // 125 }
  {0x08,0x04,0x08,0x10,0x08}, // 126 ~
};

void drawChar(int16_t x, int16_t y, char ch, uint16_t color, uint8_t scale) {
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t* glyph = FONT5X7[(uint8_t)ch - 32];
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&glyph[col]);
    for (uint8_t row = 0; row < 7; row++) {
      if (bits & (1 << row)) {
        fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

int16_t drawStr(int16_t x, int16_t y, const char* s, uint16_t color, uint8_t scale) {
  while (*s) {
    drawChar(x, y, *s++, color, scale);
    x += 6 * scale;
  }
  return x;
}

void drawStrC(int16_t cx, int16_t y, const char* s, uint16_t color, uint8_t scale) {
  int16_t w = (int16_t)(strlen(s) * 6 * scale);
  drawStr(cx - w / 2, y, s, color, scale);
}

void safeStr(const char* src, char* dst, int maxChars) {
  int len = (int)strlen(src);
  if (len <= maxChars) {
    strcpy(dst, src);
  } else {
    strncpy(dst, src, maxChars - 2);
    dst[maxChars - 2] = '.';
    dst[maxChars - 1] = '.';
    dst[maxChars]     = '\0';
  }
}

// ============================================================
//  GC9A01 INIT SEQUENCE
// ============================================================
void initDisplay() {
  digitalWrite(PIN_RST, LOW);  delay(120);
  digitalWrite(PIN_RST, HIGH); delay(120);

  dispCmd(0xEF);
  dispCmd(0xEB); dispData(0x14);
  dispCmd(0xFE);
  dispCmd(0xEF);
  dispCmd(0xEB); dispData(0x14);
  dispCmd(0x84); dispData(0x40);
  dispCmd(0x85); dispData(0xFF);
  dispCmd(0x86); dispData(0xFF);
  dispCmd(0x87); dispData(0xFF);
  dispCmd(0x88); dispData(0x0A);
  dispCmd(0x89); dispData(0x21);
  dispCmd(0x8A); dispData(0x00);
  dispCmd(0x8B); dispData(0x80);
  dispCmd(0x8C); dispData(0x01);
  dispCmd(0x8D); dispData(0x01);
  dispCmd(0x8E); dispData(0xFF);
  dispCmd(0x8F); dispData(0xFF);
  dispCmd(0xB6); dispData(0x00); dispData(0x20);
  dispCmd(0x36); dispData(0xC8);
  dispCmd(0x3A); dispData(0x05);
  dispCmd(0x90); dispData(0x08); dispData(0x08); dispData(0x08); dispData(0x08);
  dispCmd(0xBD); dispData(0x06);
  dispCmd(0xBC); dispData(0x00);
  dispCmd(0xFF); dispData(0x60); dispData(0x01); dispData(0x04);
  dispCmd(0xC3); dispData(0x13);
  dispCmd(0xC4); dispData(0x13);
  dispCmd(0xC9); dispData(0x22);
  dispCmd(0xBE); dispData(0x11);
  dispCmd(0xE1); dispData(0x10); dispData(0x0E);
  dispCmd(0xDF); dispData(0x21); dispData(0x0C); dispData(0x02);
  dispCmd(0xF0); dispData(0x45);dispData(0x09);dispData(0x08);dispData(0x08);dispData(0x26);dispData(0x2A);
  dispCmd(0xF1); dispData(0x43);dispData(0x70);dispData(0x72);dispData(0x36);dispData(0x37);dispData(0x6F);
  dispCmd(0xF2); dispData(0x45);dispData(0x09);dispData(0x08);dispData(0x08);dispData(0x26);dispData(0x2A);
  dispCmd(0xF3); dispData(0x43);dispData(0x70);dispData(0x72);dispData(0x36);dispData(0x37);dispData(0x6F);
  dispCmd(0xED); dispData(0x1B); dispData(0x0B);
  dispCmd(0xAE); dispData(0x77);
  dispCmd(0xCD); dispData(0x63);
  dispCmd(0x70); dispData(0x07);dispData(0x07);dispData(0x04);dispData(0x0E);dispData(0x0F);dispData(0x09);dispData(0x07);dispData(0x08);dispData(0x03);
  dispCmd(0xE8); dispData(0x34);
  dispCmd(0x62); dispData(0x18);dispData(0x0D);dispData(0x71);dispData(0xED);dispData(0x70);dispData(0x70);
                 dispData(0x18);dispData(0x0F);dispData(0x71);dispData(0xEF);dispData(0x70);dispData(0x70);
  dispCmd(0x63); dispData(0x18);dispData(0x11);dispData(0x71);dispData(0xF1);dispData(0x70);dispData(0x70);
                 dispData(0x18);dispData(0x13);dispData(0x71);dispData(0xF3);dispData(0x70);dispData(0x70);
  dispCmd(0x64); dispData(0x28);dispData(0x29);dispData(0xF1);dispData(0x01);dispData(0xF1);dispData(0x00);dispData(0x07);
  dispCmd(0x66); dispData(0x3C);dispData(0x00);dispData(0xCD);dispData(0x67);dispData(0x45);dispData(0x45);
                 dispData(0x10);dispData(0x00);dispData(0x00);dispData(0x00);
  dispCmd(0x67); dispData(0x00);dispData(0x3C);dispData(0x00);dispData(0x00);dispData(0x00);dispData(0x01);
                 dispData(0x54);dispData(0x10);dispData(0x32);dispData(0x98);
  dispCmd(0x74); dispData(0x10);dispData(0x85);dispData(0x80);dispData(0x00);dispData(0x00);dispData(0x4E);dispData(0x00);
  dispCmd(0x98); dispData(0x3E); dispData(0x07);
  dispCmd(0x11); delay(120);
  dispCmd(0x29);
}

// ============================================================
//  STATUS COLOR
// ============================================================
uint16_t statusColor(const char* s) {
  String st(s);
  st.toLowerCase();
  if (st.indexOf("boarding")    >= 0) return C_GREEN;
  if (st.indexOf("delayed")     >= 0) return C_RED;
  if (st.indexOf("gate change") >= 0) return C_YELLOW;
  if (st.indexOf("final")       >= 0) return C_ORANGE;
  return C_ACCENT;
}

// ============================================================
//  SMALL DECORATIVE WIDGETS
// ============================================================
void drawWifiDot(int16_t x, int16_t y, bool connected) {
  uint16_t c = connected ? C_GREEN : C_GRAY;
  drawCircle(x, y, 7, c);
  drawCircle(x, y, 4, c);
  fillCircle(x, y, 2, c);
}

void drawPlaneTiny(int16_t x, int16_t y, uint16_t c) {
  fillRect(x+3, y+3, 9, 3, c);
  fillRect(x+11,y+4, 4, 1, c);
  fillRect(x+4, y+1, 5, 1, c);
  fillRect(x+4, y+6, 5, 1, c);
  fillRect(x+2, y+2, 2, 1, c);
  fillRect(x+2, y+6, 2, 1, c);
}

// ============================================================
//  SCREEN: SPLASH
// ============================================================
void drawSplash() {
  fillScreen(C_BG);
  for (int r = 115; r >= 110; r--) drawCircle(120, 120, r, C_ACCENT);
  for (int r = 105; r >= 103; r--) drawCircle(120, 120, r, C_DKGRAY);
  fillRect(68,  116, 100, 8,  C_PLANE);
  fillRect(166, 117,   8, 6,  C_PLANE);
  fillRect(173, 119,   4, 2,  C_PLANE);
  fillRect(92,  94,  50, 5,  C_PLANE);
  fillRect(78,  98,  14, 22, C_PLANE);
  fillRect(70,  106, 18, 5,  C_PLANE);
  fillRect(70,  127, 18, 5,  C_PLANE);
  drawStrC(120, 155, "TRAVEL BAND", C_ACCENT, 2);
  drawStrC(120, 178, "ATAB-001",    C_GRAY,   2);
  drawStrC(120, 200, "Connecting..",C_GRAY,   1);
}

// ============================================================
//  SCREEN: NO WIFI
// ============================================================
void drawNoWifi() {
  fillScreen(C_BG);
  drawCircle(120, 120, 115, C_DKGRAY);
  drawStrC(120,  75, "NO WIFI",      C_RED,    2);
  drawStrC(120, 105, "Check SSID",   C_GRAY,   1);
  drawStrC(120, 120, "& Password",   C_GRAY,   1);
  drawStrC(120, 148, WIFI_SSID,      C_YELLOW, 1);
  drawStrC(120, 165, "Retrying...",  C_GRAY,   1);
}

// ============================================================
//  SCREEN: LIST VIEW
// ============================================================
void drawListView() {
  fillScreen(C_BG);

  fillRect(0, 0, 240, 24, C_PANEL);
  drawStrC(110, 7, "FLIGHT ALERTS", C_ACCENT, 1);
  drawWifiDot(225, 12, WiFi.status() == WL_CONNECTED);
  drawPlaneTiny(6, 5, C_ACCENT);

  fillRect(0, 216, 240, 24, C_PANEL);
  drawStr(  8, 222, "<PREV",  C_GRAY, 1);
  drawStrC(120, 222, "SELECT", C_GRAY, 1);
  drawStr(187, 222, "NEXT>",  C_GRAY, 1);

  if (alertCount == 0) {
    drawStrC(120,  98, "No alerts yet.",      C_GRAY, 1);
    drawStrC(120, 114, "Waiting for server.", C_GRAY, 1);
    return;
  }

  const int ROW_START = 28;
  const int ROW_H     = 37;
  const int MAX_VIS   = 5;

  int scrollOff = 0;
  if (selectedIndex >= MAX_VIS) scrollOff = selectedIndex - MAX_VIS + 1;

  for (int i = 0; i < MAX_VIS; i++) {
    int idx = i + scrollOff;
    if (idx >= alertCount) break;

    FlightAlert& a   = alerts[idx];
    int16_t      y   = ROW_START + i * ROW_H;
    bool         sel = (idx == selectedIndex);

    if (sel) {
      fillRect(6, y, 228, ROW_H - 3, C_DKGRAY);
      fillRect(6, y,   3, ROW_H - 3, C_ACCENT);
    }

    char fl[12]; safeStr(a.flight, fl, 7);
    drawStr(14, y + 3, fl, sel ? C_WHITE : C_ACCENT, 2);

    char ds[16]; safeStr(a.destination, ds, 9);
    drawStr(82, y + 3, ds, sel ? C_WHITE : C_GRAY, 1);

    drawStr(82, y + 16, a.flightTime, C_YELLOW, 1);

    char gl[10]; snprintf(gl, sizeof(gl), "G:%s", a.gate);
    drawStr(148, y + 16, gl, C_ACCENT, 1);

    fillCircle(220, y + 12, 5, statusColor(a.status));

    if (!sel) drawHLine(6, y + ROW_H - 4, 228, C_DKGRAY);
  }

  if (alertCount > 1) {
    int n    = min(alertCount, 10);
    int dotX = 120 - (n * 9) / 2;
    for (int i = 0; i < n; i++) {
      if (i == selectedIndex)
        fillCircle(dotX + i * 9, 211, 3, C_ACCENT);
      else
        fillCircle(dotX + i * 9, 211, 2, C_DKGRAY);
    }
  }
}

// ============================================================
//  SCREEN: DETAIL VIEW
// ============================================================
void drawDetailView() {
  fillScreen(C_BG);
  FlightAlert& a = alerts[selectedIndex];

  fillRect(0, 0, 240, 30, C_PANEL);
  char hdr[20]; snprintf(hdr, sizeof(hdr), ">> %s <<", a.flight);
  drawStrC(120, 8, hdr, C_ACCENT, 2);

  drawCircle(120, 120, 116, C_DKGRAY);

  uint16_t sc = statusColor(a.status);
  fillRect(0, 32, 240, 4, sc);

  drawStrC(120, 46, "DESTINATION", C_GRAY,  1);
  char ds[20]; safeStr(a.destination, ds, 13);
  drawStrC(120, 60, ds, C_WHITE, 2);

  drawHLine(18, 88, 204, C_DKGRAY);

  drawStrC( 68, 95, "TIME", C_GRAY, 1);
  drawStrC(172, 95, "GATE", C_GRAY, 1);
  drawVLine(120, 91, 55, C_DKGRAY);
  drawStrC( 68, 110, a.flightTime, C_YELLOW, 2);
  drawStrC(172, 110, a.gate,       C_ACCENT, 2);

  drawHLine(18, 148, 204, C_DKGRAY);

  drawStrC(120, 155, "STATUS", C_GRAY, 1);
  char st[20]; safeStr(a.status, st, 13);
  drawStrC(120, 170, st, sc, 2);

  fillRect(0, 206, 240, 34, C_PANEL);
  drawStr(10, 215, "<BACK", C_GRAY, 1);

  char pg[12]; snprintf(pg, sizeof(pg), "%d/%d", selectedIndex+1, alertCount);
  drawStrC(120, 215, pg, C_DKGRAY, 1);

  drawStr(182, 215, "P/N>", C_GRAY, 1);
}

// ============================================================
//  FETCH ALERTS FROM SERVER
// ============================================================
void fetchAlerts() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Not connected, skipping fetch.");
    return;
  }

  char url[160];
  snprintf(url, sizeof(url), "%s/devices/%s/notifications", SERVER_BASE, DEVICE_ID);
  Serial.printf("[HTTP] GET %s\n", url);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  Serial.printf("[HTTP] Response: %d\n", code);

  if (code == 200) {
    String payload = http.getString();
    Serial.printf("[JSON] Payload length: %d bytes\n", payload.length());

    StaticJsonDocument<4096> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[JSON] Parse error: %s\n", err.c_str());
      http.end();
      return;
    }

    JsonArray arr   = doc["notifications"].as<JsonArray>();
    int       count = (int)arr.size();
    Serial.printf("[JSON] Notifications in response: %d\n", count);

    if (count == 0) { http.end(); return; }

    int newestId = arr[count - 1]["id"] | 0;

    if (newestId == lastServerId && alertCount > 0) {
      Serial.println("[Fetch] No new alerts.");
      http.end();
      return;
    }

    lastServerId = newestId;
    alertCount   = 0;

    for (int i = count - 1; i >= 0 && alertCount < MAX_ALERTS; i--) {
      JsonObject obj = arr[i].as<JsonObject>();
      FlightAlert& out = alerts[alertCount];
      out.id = obj["id"] | 0;

      const char* msg = obj["message"] | "";
      char buf[128];
      strncpy(buf, msg, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      char* parts[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
      int   pi    = 0;
      char* token = strtok(buf, "|");
      while (token && pi < 5) {
        while (*token == ' ') token++;
        int tlen = (int)strlen(token);
        while (tlen > 0 && token[tlen-1] == ' ') token[--tlen] = '\0';
        parts[pi++] = token;
        token = strtok(nullptr, "|");
      }

      strncpy(out.flight,      parts[0] ? parts[0] : "----",  sizeof(out.flight)      - 1);
      strncpy(out.destination, parts[1] ? parts[1] : "---",   sizeof(out.destination) - 1);
      strncpy(out.flightTime,  parts[2] ? parts[2] : "--:--", sizeof(out.flightTime)  - 1);
      strncpy(out.gate,        parts[3] ? parts[3] : "--",    sizeof(out.gate)        - 1);
      strncpy(out.status,      parts[4] ? parts[4] : "---",   sizeof(out.status)      - 1);

      out.flight[sizeof(out.flight)-1]           = '\0';
      out.destination[sizeof(out.destination)-1] = '\0';
      out.flightTime[sizeof(out.flightTime)-1]   = '\0';
      out.gate[sizeof(out.gate)-1]               = '\0';
      out.status[sizeof(out.status)-1]           = '\0';

      Serial.printf("[Alert %d] %s | %s | %s | %s | %s\n",
        out.id, out.flight, out.destination,
        out.flightTime, out.gate, out.status);

      alertCount++;
    }

    selectedIndex = 0;
    needsRedraw   = true;
    Serial.printf("[Fetch] Stored %d alerts. Newest ID=%d\n", alertCount, newestId);

    // ── NEW: Fire vibration + audio on new alert data ─────
    triggerAlertFeedback();

  } else {
    Serial.printf("[HTTP] Error code: %d\n", code);
  }

  http.end();
}

// ============================================================
//  BUTTON STATE
// ============================================================
static bool     gPrevLast = HIGH,  gNextLast = HIGH,  gSelLast = HIGH;
static uint32_t gPrevTime = 0,     gNextTime = 0,     gSelTime = 0;

bool btnFell(uint8_t pin, bool& lastState, uint32_t& lastTime) {
  bool cur = (bool)digitalRead(pin);
  if (cur == LOW && lastState == HIGH && (millis() - lastTime) > 50UL) {
    lastState = LOW;
    lastTime  = millis();
    return true;
  }
  if (cur == HIGH) lastState = HIGH;
  return false;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n==============================");
  Serial.println(" ATAB Band Firmware Starting");
  Serial.println("==============================");

  // Display pins
  pinMode(PIN_CS,  OUTPUT); digitalWrite(PIN_CS,  HIGH);
  pinMode(PIN_DC,  OUTPUT); digitalWrite(PIN_DC,  HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);

  // SPI + display init
  SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  initDisplay();
  Serial.println("[Display] GC9A01 initialized.");

  // Button pins
  pinMode(BTN_PREV,   INPUT_PULLUP);
  pinMode(BTN_NEXT,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  Serial.println("[Buttons] GPIO 12=PREV  13=NEXT  14=SELECT");

  // ── NEW: Vibration motor pin ────────────────────────────
  pinMode(PIN_VIBRO, OUTPUT);
  digitalWrite(PIN_VIBRO, LOW);
  Serial.println("[Vibro] GPIO 27 initialized.");

  // ── NEW: DFMiniPlayer init on UART2 ─────────────────────
  dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
  delay(1000);   // DFPlayer needs ~1s to boot
  if (dfPlayer.begin(dfSerial)) {
    dfPlayer.volume(25);   // 0–30, adjust to taste
    Serial.println("[DFPlayer] Initialized. Volume=25.");
  } else {
    Serial.println("[DFPlayer] Init FAILED — check wiring/SD card.");
  }

  // Splash screen while WiFi connects
  drawSplash();

  // WiFi
  Serial.printf("[WiFi] Connecting to: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 24) {
    delay(500);
    tries++;
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Server: %s\n", SERVER_BASE);
    fetchAlerts();
  } else {
    Serial.println("[WiFi] FAILED. Check SSID and password.");
    drawNoWifi();
    delay(3000);
  }

  needsRedraw = true;
}

// ============================================================
//  LOOP
// ============================================================
static unsigned long gLastFetch = 0;
static const unsigned long FETCH_MS = 8000UL;

void loop() {
  if (millis() - gLastFetch >= FETCH_MS) {
    gLastFetch = millis();
    fetchAlerts();
  }

  bool redraw = false;

  if (btnFell(BTN_PREV, gPrevLast, gPrevTime)) {
    Serial.println("[BTN] PREV pressed");
    if (detailView) {
      if (selectedIndex < alertCount - 1) { selectedIndex++; redraw = true; }
    } else {
      if (selectedIndex < alertCount - 1) { selectedIndex++; redraw = true; }
    }
  }

  if (btnFell(BTN_NEXT, gNextLast, gNextTime)) {
    Serial.println("[BTN] NEXT pressed");
    if (detailView) {
      if (selectedIndex > 0) { selectedIndex--; redraw = true; }
    } else {
      if (selectedIndex > 0) { selectedIndex--; redraw = true; }
    }
  }

  if (btnFell(BTN_SELECT, gSelLast, gSelTime)) {
    Serial.println("[BTN] SELECT pressed");
    if (alertCount > 0) {
      detailView = !detailView;
      Serial.printf("[UI] Switched to %s view\n", detailView ? "DETAIL" : "LIST");
      redraw = true;
    }
  }

  if (redraw || needsRedraw) {
    needsRedraw = false;
    if (detailView && alertCount > 0) {
      Serial.printf("[Draw] Detail view, alert %d/%d\n", selectedIndex+1, alertCount);
      drawDetailView();
    } else {
      Serial.printf("[Draw] List view, %d alerts, selected=%d\n", alertCount, selectedIndex);
      drawListView();
    }
  }

  delay(20);
}