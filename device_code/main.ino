#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

#define USE_MIC 0   // set to 1 only when SPH0645 is wired
#define USE_RING 1  // ring on/off

#if USE_MIC
  #include <driver/i2s.h>
  #include <soc/i2s_reg.h>
#endif

// ------- Display timing (stable) -------
#define PCLK_HZ         14000000
#define PCLK_ACTIVE_NEG 1
#define H_FRONT   24
#define H_PULSE    4
#define H_BACK    64
#define V_FRONT   12
#define V_PULSE    2
#define V_BACK    20
// --------------------------------------

// ------- UI layout -------
const int VU_W = 44;
const int UI_RIGHT_H = 140;
const int UI_RIGHT_Y = 320 - UI_RIGHT_H;

const int RING_SZ = 192;                      // smaller = faster (96x96 ≈ 18 KB buffer)
const int RING_RO = 96;                      // outer radius   (<= RING_SZ/2 - margin)
const int RING_RI = 72;                      // inner radius
const int RING_CY = 160;
const int RING_CX = 650; // TODO: we went from 720 to 650, adjust the text
const int RING_X  = RING_CX - RING_SZ/2;     // on-screen placement
const int RING_Y  = RING_CY - RING_SZ/2;

const int TXT_Y = UI_RIGHT_Y - 64;
const int TXT_X = RING_CX - RING_RO - 512;    // baseline for size=2 text
// ------------------------

// ------- Countdown -------
static const uint32_t COUNTDOWN_SECONDS = 10 * 60;
// ------------------------

// Qualia expander
Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
  PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK, PCA_TFT_MOSI, &Wire, 0x3F);

// RGB666 panel
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
  TFT_R1, TFT_R2, TFT_R3, TFT_R4, TFT_R5,
  TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
  TFT_B1, TFT_B2, TFT_B3, TFT_B4, TFT_B5,
  1, H_FRONT, H_PULSE, H_BACK,
  1, V_FRONT, V_PULSE, V_BACK,
  PCLK_ACTIVE_NEG, PCLK_HZ
);

// auto_flush = true (immediate writes; no big flush copies)
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  320, 960, rgbpanel, 0, true,
  expander, GFX_NOT_DEFINED,
  HD458002C40_init_operations, sizeof(HD458002C40_init_operations),
  80
);

// ---------- Helpers ----------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline uint16_t hex565(uint32_t rgb) {     // 0xRRGGBB
  return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static void draw_grid_left(uint16_t color, int dx, int dy) {
  for (int x = 0; x < UI_RIGHT_Y; x += dx) gfx->drawFastVLine(x, 0, gfx->height(), color);
  for (int y = 0; y < gfx->height(); y += dy) gfx->drawFastHLine(0, y, UI_RIGHT_Y, color);
}
static void fmt_hhmmss(uint32_t sec, char *out) {
  uint32_t m = sec / 60, s = sec % 60;
  uint32_t h = m / 60;
  m = m % 60;
  sprintf(out, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

#if USE_MIC
static void i2s_init() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = { 4, 17, -1, 7 };
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
  REG_SET_BIT(I2S_RX_TIMING_REG(I2S_NUM_0), BIT(1));
  REG_SET_BIT(I2S_RX_CONF1_REG(I2S_NUM_0), I2S_RX_MSB_SHIFT);
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
}
#endif

// ---------- Timer state ----------
static uint32_t countdown_left = COUNTDOWN_SECONDS;
static uint32_t last_second_ms = 0;
static char last_text[9] = "--------";
static char last_name[16] = "---------------";
// ---------------------------------

// ---------- Ring LUT + buffer (no Arduino_Canvas!) ----------
#if USE_RING
static uint16_t ringbuf[RING_SZ * RING_SZ];        // RGB565 bitmap to blit
static uint16_t angleLUT[RING_SZ * RING_SZ];       // angle in [0..65535]
static uint8_t  maskLUT[RING_SZ * RING_SZ];        // 1 = in donut, 0 = outside

static void init_ring_lut() {
  const float SCALE  = 65535.0f / TWO_PI;
  const int cx = RING_SZ / 2;
  const int cy = RING_SZ / 2;
  const int ro2 = RING_RO * RING_RO;
  const int ri2 = RING_RI * RING_RI;
  for (int y = 0; y < RING_SZ; y++) {
    int dy = y - cy;
    int dy2 = dy * dy;
    for (int x = 0; x < RING_SZ; x++) {
      int dx = x - cx;
      int r2 = dx*dx + dy2;
      int idx = y * RING_SZ + x;
      if (r2 <= ro2 && r2 >= ri2) {
        float theta = atan2f((float)dx, (float)(-dy)); // cw angle from 12 o'clock
        if (theta < 0) theta += TWO_PI;
        angleLUT[idx] = (uint16_t)(theta * SCALE + 0.5f);
        maskLUT[idx]  = 1;
      } else {
        maskLUT[idx]  = 0;
      }
    }
  }
}

static void draw_ring(float fracRemaining) {
  // fracRemaining: 1.0 = full time left, 0.0 = time up
  if (fracRemaining < 0) fracRemaining = 0;
  if (fracRemaining > 1) fracRemaining = 1;

  // threshold in [0..65535]
  uint16_t threshold = (uint16_t)(fracRemaining * 65535.0f + 0.5f);
  const uint16_t BG = DARKGREY; // TODO: replace with color in between background and ring
  const uint16_t FG = WHITE; // TODO: replace with light blue gradient from flutter

  // Reverse direction: our LUT angles are clockwise-from-12.
  // To fill CCW, we keep pixels whose CW angle >= (FULL - threshold).
  uint16_t cut = (uint16_t)(65535 - threshold);

  for (int i = 0; i < RING_SZ * RING_SZ; i++) {
    if (!maskLUT[i]) {
      ringbuf[i] = hex565(0x14215E);                 // outside the donut
      // ringbuf[i] = BLACK; // debug only
    } else {
      ringbuf[i] = (angleLUT[i] >= cut) ? FG : BG;
    }
  }

  // Single compact blit
  gfx->draw16bitRGBBitmap(RING_X, RING_Y, ringbuf, RING_SZ, RING_SZ);
}

#endif
// -------------------------------------------------------------

void setup() {
  Wire.setClock(1000000);
  gfx->begin();
  gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  // Static background (left only)
  gfx->fillScreen(hex565(0x14215E)); // TODO: replace with 14215E
//   const uint16_t cols[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE };
//   int bh = gfx->height() / 7;
//   for (int i = 0; i < 7; i++) gfx->fillRect(0, i*bh, UI_RIGHT_Y, bh, cols[i]);
//   draw_grid_left(DARKGREY, 16, 16);

//   // Right panel once (we won't clear it every frame)
//   gfx->fillRect(UI_RIGHT_Y, 0, UI_RIGHT_H, gfx->height(), BLACK);

  // VU frame once
  gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);

  // Timer text style: foreground + background (!) so we don't clear big rects
  gfx->setTextSize(8);
  gfx->setTextWrap(false);
  gfx->setTextColor(WHITE, hex565(0x14215E));

  #if USE_RING
    init_ring_lut();   // heavy math done once
  #endif

  #if USE_MIC
    i2s_init();
  #endif

  // Initial timer draw
  last_second_ms = millis();
  char hhmmss[9]; 
  char timer_name[16];

  fmt_hhmmss(countdown_left, hhmmss);
  strcpy(last_text, hhmmss);
  gfx->setCursor(TXT_X, TXT_Y);
  gfx->print(hhmmss);

  // Write the timer name above the time
  sprintf(timer_name, "Countdown Timer");
  strcpy(last_name, timer_name);
  gfx->setTextSize(4);
  gfx->setCursor(TXT_X - 60, TXT_Y - 40);
  gfx->print(timer_name);
}

void loop() {
  uint32_t now = millis();

// --- Update countdown once per second (you already have this) ---
if (now - last_second_ms >= 1000) { // if a second has passed
  last_second_ms += 1000;
  countdown_left = (countdown_left > 0) ? (countdown_left - 1) : COUNTDOWN_SECONDS;

  // Update the text once per second (you already do this)
  char hhmmss[9]; fmt_hhmmss(countdown_left, hhmmss);
  char timer_name[16]; sprintf(timer_name, "Countdown Timer");

  if (strcmp(hhmmss, last_text) != 0) {
    strcpy(last_text, hhmmss);
    gfx->setTextSize(8);
    gfx->setCursor(TXT_X, TXT_Y);
    gfx->print(hhmmss);  // white text with BLACK bg; tiny overwrite only
  }

  if (strcmp(timer_name, last_name) != 0) {
    strcpy(last_name, timer_name);
    gfx->setTextSize(4);
    gfx->setCursor(TXT_X - 60, TXT_Y - 40);
    gfx->print(timer_name);
  }
}

// --- Animate ring ~30 FPS, tied to whole countdown ---
#if USE_RING
static uint32_t last_ring_ms = 0;
if (now - last_ring_ms >= 100) {                 // 33 --> ~30 FPS
  last_ring_ms = now;

  // fractional seconds within the current second
  float sub = (float)(now - last_second_ms) / 1000.0f;
  if (sub < 0) sub = 0; if (sub > 1) sub = 1;

  // exact seconds remaining, including sub-second
  float remainingExact = (float)countdown_left + (1.0f - sub);
  if (remainingExact < 0) remainingExact = 0;

  // fraction of total time remaining (1 → 0 over the entire 10 minutes)
  float fracRemaining = remainingExact / (float)COUNTDOWN_SECONDS;

  draw_ring(fracRemaining);
}
#endif


  delay(2);
}
