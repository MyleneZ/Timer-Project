/*
 * Qualia Font Tester — Racing Sans One
 *
 * Brings up the Adafruit Qualia ESP32-S3 RGB666 display using the same
 * Arduino_GFX configuration as:
 *   - device_code/production.ino
 *   - device_code/gif_tester.ino
 *
 * Then renders the RacingSansOne_Regular20pt7b GFX font at a few sizes and
 * draws text bounding boxes to help validate alignment.
 *
 * Controls (Serial @ 115200):
 *   - Send '0', '1', or '2' to switch pages
 *   - Pages auto-advance every few seconds
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "font/RacingSansOne_Regular20pt7b.h"

// ======================= DISPLAY CONFIG (from production.ino) =======================
#define PCLK_HZ         12000000
#define PCLK_ACTIVE_NEG 1
#define H_FRONT   24
#define H_PULSE    4
#define H_BACK    64
#define V_FRONT   12
#define V_PULSE    2
#define V_BACK    20

Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
  PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK, PCA_TFT_MOSI, &Wire, 0x3F);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
  TFT_R1, TFT_R2, TFT_R3, TFT_R4, TFT_R5,
  TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
  TFT_B1, TFT_B2, TFT_B3, TFT_B4, TFT_B5,
  1, H_FRONT, H_PULSE, H_BACK,
  1, V_FRONT, V_PULSE, V_BACK,
  PCLK_ACTIVE_NEG, PCLK_HZ
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  320, 960, rgbpanel, 0, true,
  expander, GFX_NOT_DEFINED,
  HD458002C40_init_operations, sizeof(HD458002C40_init_operations),
  80
);

// ======================= COLORS =======================
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t hex565(uint32_t rgb) {
  return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static const uint16_t COL_BG = hex565(0x181a20);
static const uint16_t COL_GRID = hex565(0x2b3040);
static const uint16_t COL_TEXT = WHITE;
static const uint16_t COL_MUTED = hex565(0x99a2b1);
static const uint16_t COL_BOX = hex565(0x7fbcff);
static const uint16_t COL_BASELINE = hex565(0xff9c5a);

// ======================= FONT HELPERS =======================
static void ui_font_set(uint8_t scale) {
  gfx->setFont(&RacingSansOne_Regular20pt7b);
  gfx->setTextSize(scale);
  gfx->setTextWrap(false);
}

static void ui_font_reset() {
  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  gfx->setTextWrap(true);
}

static void ui_text_bounds(const char *text, uint8_t scale,
                           int16_t *x1, int16_t *y1,
                           uint16_t *w, uint16_t *h) {
  ui_font_set(scale);
  gfx->getTextBounds(text, 0, 0, x1, y1, w, h);
  ui_font_reset();
}

static void ui_draw_text_boxed(const char *text, int left, int top,
                               uint16_t text_color, uint8_t scale,
                               bool draw_box, bool draw_baseline) {
  int16_t x1, y1;
  uint16_t w, h;
  ui_text_bounds(text, scale, &x1, &y1, &w, &h);

  if (draw_box && w && h) {
    gfx->drawRect(left, top, (int)w, (int)h, COL_BOX);
    gfx->fillRect(left - 2, top - 2, 5, 5, COL_BOX); // anchor marker
  }
  if (draw_baseline && w) {
    const int baseline_y = top - (int)y1;
    gfx->drawFastHLine(left, baseline_y, (int)w, COL_BASELINE);
  }

  ui_font_set(scale);
  gfx->setTextColor(text_color);
  gfx->setCursor(left - (int)x1, top - (int)y1);
  gfx->print(text);
  ui_font_reset();

  Serial.printf("[FONT] \"%s\" scale=%u bounds: x1=%d y1=%d w=%u h=%u (top-left=%d,%d)\n",
                text, (unsigned)scale, (int)x1, (int)y1, (unsigned)w, (unsigned)h, left, top);
}

static void ui_draw_text_boxed(const char *text, int left, int top,
                               uint16_t text_color, uint8_t scale) {
  ui_draw_text_boxed(text, left, top, text_color, scale, true, true);
}

static void ui_draw_text_centered(const char *text, int center_x, int top,
                                  uint16_t text_color, uint8_t scale) {
  int16_t x1, y1;
  uint16_t w, h;
  ui_text_bounds(text, scale, &x1, &y1, &w, &h);
  const int left = center_x - ((int)w / 2);
  ui_draw_text_boxed(text, left, top, text_color, scale, true, true);
}

// ======================= RENDERING =======================
static void draw_grid(int dx, int dy) {
  const int w = gfx->width();
  const int h = gfx->height();
  for (int x = 0; x < w; x += dx) gfx->drawFastVLine(x, 0, h, COL_GRID);
  for (int y = 0; y < h; y += dy) gfx->drawFastHLine(0, y, w, COL_GRID);
  gfx->drawRect(0, 0, w, h, COL_GRID);
}

static void render_page(uint8_t page) {
  gfx->fillScreen(COL_BG);
  draw_grid(80, 40);

  // Header (default built-in font)
  gfx->setTextColor(COL_MUTED);
  gfx->setTextSize(1);
  gfx->setTextWrap(false);
  gfx->setCursor(12, 12);
  gfx->printf("Racing Sans One font tester  |  page %u/2  |  send 0/1/2 over Serial", (unsigned)page);

  const int w = gfx->width();

  switch (page) {
    case 0: {
      ui_draw_text_boxed("Racing Sans One", 24, 44, COL_TEXT, 1);
      ui_draw_text_boxed("The quick brown fox", 24, 92, COL_TEXT, 1);
      ui_draw_text_boxed("jumps over 13 lazy dogs.", 24, 128, COL_TEXT, 1);

      // Similar to production UI usage
      ui_draw_text_boxed("12:34:56", 24, 176, COL_TEXT, 2);
      ui_draw_text_boxed("00:01:05", 24, 248, COL_TEXT, 2);
      break;
    }
    case 1: {
      // Centering + large scale
      ui_draw_text_centered("12:34:56", w / 2, 70, COL_TEXT, 3);
      ui_draw_text_centered("Centered label", w / 2, 190, COL_TEXT, 1);
      ui_draw_text_centered("Baking  Break  Homework", w / 2, 240, COL_TEXT, 1);
      break;
    }
    case 2:
    default: {
      ui_draw_text_boxed("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 24, 60, COL_TEXT, 1);
      ui_draw_text_boxed("abcdefghijklmnopqrstuvwxyz", 24, 108, COL_TEXT, 1);
      ui_draw_text_boxed("0123456789  :;!?  +-*/()[]", 24, 156, COL_TEXT, 1);
      ui_draw_text_boxed("~@#$%^&_=|\\\\<>", 24, 204, COL_TEXT, 1);
      ui_draw_text_boxed("WMWMWM iiiii  88:88:88", 24, 252, COL_TEXT, 1);
      break;
    }
  }
}

// ======================= APP LOOP =======================
static uint8_t g_page = 0;
static uint32_t g_last_page_ms = 0;
static const uint32_t PAGE_MS = 6500;

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] Racing Sans One font tester");

  Wire.setClock(1000000);
  gfx->begin();
  gfx->setRotation(1); // landscape (960x320), matches production.ino

  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  Serial.println("[BOOT] Display initialized. Pages auto-advance; send 0/1/2 to switch.");

  g_page = 0;
  g_last_page_ms = millis();
  render_page(g_page);
}

void loop() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c >= '0' && c <= '2') {
      g_page = (uint8_t)(c - '0');
      g_last_page_ms = millis();
      render_page(g_page);
    }
  }

  const uint32_t now = millis();
  if ((now - g_last_page_ms) >= PAGE_MS) {
    g_page = (uint8_t)((g_page + 1) % 3);
    g_last_page_ms = now;
    render_page(g_page);
  }
}
