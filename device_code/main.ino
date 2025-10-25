#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <vector>

#define USE_MIC 1   // set to 1 only when SPH0645 is wired
#define USE_RING 1  // ring on/off
#define USE_SPEAKER 0 // for playing SFX

#if USE_MIC
  #include <driver/i2s.h>
  // #include <soc/i2s_reg.h>
  // #include <I2S.h>
  #define I2S_PORT I2S_NUM_0
  #define PIN_BCLK  SCK   // Qualia SCK header pin
  #define PIN_WS    A0    // Qualia A0
  #define PIN_SD    A1    // Qualia A1
#endif

// ------- Display timing (stable) -------
#define PCLK_HZ         12000000
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

const int TXT_Y = UI_RIGHT_Y - 32;
const int TXT_X = RING_CX - RING_RO - 412;    // baseline for size=2 text
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
static inline uint16_t lerp565(uint16_t c0, uint16_t c1, uint8_t t /*0..255*/) {
  // work directly in 5/6/5 space to keep it fast
  uint16_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  uint16_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint16_t r = (uint16_t)((r0 * (255 - t) + r1 * t) / 255);
  uint16_t g = (uint16_t)((g0 * (255 - t) + g1 * t) / 255);
  uint16_t b = (uint16_t)((b0 * (255 - t) + b1 * t) / 255);
  return (r << 11) | (g << 5) | b;
}
// Gradient endpoints (t=0 -> start, t=255 -> end)
static uint16_t GRAD_START = hex565(0xe9faff);   // light blue (trail)
static uint16_t GRAD_END   = hex565(0x0099ff);   // deep blue  (lead)
static uint16_t GRAD_BLUE   = hex565(0x0099ff);   // deep blue  (lead)
static uint16_t GRAD_RED   = hex565(0xFE809F); 
static uint16_t GRAD_GREEN   = hex565(0x59CEB9); 
static uint16_t GRAD_PURPLE   = hex565(0xC481FF); 
static uint16_t GRAD_ORANGE   = hex565(0xFF845B); 
static bool     invertGradient = false;          // set true to reverse


static void draw_grid_left(uint16_t color, int dx, int dy) {
  for (int x = 0; x < UI_RIGHT_Y; x += dx) gfx->drawFastVLine(x, 0, gfx->height(), color);
  for (int y = 0; y < gfx->height(); y += dy) gfx->drawFastHLine(0, y, UI_RIGHT_Y, color);
}
static void fmt_hhmmss(uint32_t sec, char *out, uint16_t color) {
  gfx->setTextColor(WHITE, color);
  uint32_t m = sec / 60, s = sec % 60;
  uint32_t h = m / 60;
  m = m % 60;
  sprintf(out, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

#if USE_MIC

// Voice Command Variables
enum TokenId : uint8_t {
  TK_SET, TK_CANCEL, TK_ADD, TK_MINUS, TK_STOP, TK_TIMER,
  TK_MINUTE, TK_MINUTES, TK_HOUR, TK_HOURS,
  TK_ONE, TK_TWO, TK_THREE, TK_FOUR, TK_FIVE, TK_SIX, TK_SEVEN, TK_EIGHT, TK_NINE,
  TK_TEN, TK_ELEVEN, TK_TWELVE, TK_THIRTEEN, TK_FOURTEEN, TK_FIFTEEN, TK_SIXTEEN, TK_SEVENTEEN, TK_EIGHTEEN, TK_NINETEEN,
  TK_TWENTY, TK_THIRTY, TK_FORTY, TK_FIFTY, TK_SIXTY, TK_SEVENTY, TK_EIGHTY, TK_NINETY,
  TK_BAKING, TK_COOKING, TK_BREAK, TK_HOMEWORK, TK_EXERCISE, TK_WORKOUT
};

// Voice Command Helper Functions
// static int token_to_number(TokenId token) {
//   switch (token) {
//     case TK_ONE: return 1;
//     case TK_TWO: return 2;
//     case TK_THREE: return 3;
//     case TK_FOUR: return 4;
//     case TK_FIVE: return 5;
//     case TK_SIX: return 6;
//     case TK_SEVEN: return 7;
//     case TK_EIGHT: return 8;
//     case TK_NINE: return 9;
//     case TK_TEN: return 10;
//     case TK_ELEVEN: return 11;
//     case TK_TWELVE: return 12;
//     case TK_THIRTEEN: return 13;
//     case TK_FOURTEEN: return 14;
//     case TK_FIFTEEN: return 15;
//     case TK_SIXTEEN: return 16;
//     case TK_SEVENTEEN: return 17;
//     case TK_EIGHTEEN: return 18;
//     case TK_NINETEEN: return 19;
//     case TK_TWENTY: return 20;
//     case TK_THIRTY: return 30;
//     case TK_FORTY: return 40;
//     case TK_FIFTY: return 50;
//     case TK_SIXTY: return 60;
//     case TK_SEVENTY: return 70;
//     case TK_EIGHTY: return 80;
//     case TK_NINETY: return 90;
//     default: return 0;
//   }
// }

// static void process_voice_command(const std::vector<TokenId>& tokens) {
//   // Determine command type
//   int commandType = -1;
//   for (TokenId token : tokens) {
//     if (token == TK_SET) {
//       commandType = 0;
//     } else if (token == TK_CANCEL || token == TK_STOP) {
//       commandType = 1;
//     } else if (token == TK_ADD) {
//       commandType = 2;
//     } else if (token == TK_MINUS) {
//       commandType = 3;
//     }
//   }

//   if (commandType == -1) {
//     return;
//   }


//   // Set Command Handling

//   // Cancel/Stop Command Handling

//   // Add Command Handling

//   // Minus Command Handling

// }


/* SPH0645 I2S mic wiring to the Qualia
  LRCL -> A0
  BCLK -> SCK
  DOUT -> A1
  GND -> GND
  3V -> 3.3V
*/

// static void i2s_init() {
//   i2s_config_t cfg = {
//     .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
//     .sample_rate = 16000,
//     .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
//     .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
//     .communication_format = I2S_COMM_FORMAT_STAND_I2S,
//     .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
//     .dma_buf_count = 8,
//     .dma_buf_len = 256,
//     .use_apll = false,
//     .tx_desc_auto_clear = false,
//     .fixed_mclk = 0
//   };
//   i2s_pin_config_t pins = { 4, 17, -1, 7 };
//   ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
//   REG_SET_BIT(I2S_RX_TIMING_REG(I2S_NUM_0), BIT(1));
//   REG_SET_BIT(I2S_RX_CONF1_REG(I2S_NUM_0), I2S_RX_MSB_SHIFT);
//   ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
// }
#endif

// Timer array
// ------- Countdown -------
static const uint32_t COUNTDOWN_SECONDS_1 = 10 * 60;
static const uint32_t COUNTDOWN_SECONDS_2 = 5 * 60;
static const uint32_t COUNTDOWN_SECONDS_3 = 5;
// ------------------------
int timer_array[3] = {COUNTDOWN_SECONDS_1, COUNTDOWN_SECONDS_2, COUNTDOWN_SECONDS_3};

// ---------- Timer 1 State ----------
static char timer_name_1[16] = "Timer 1";
static uint32_t countdown_left = timer_array[0];
static uint32_t last_second_ms = 0;
static uint32_t last_ring_ms = 0;
static char last_text[9] = "--------";
static char last_name[16] = "---------------";
// ---------------------------------

// ---------- Timer 2 State ----------
static char timer_name_2[16] = "Timer 2";
static uint32_t countdown_left_2 = timer_array[1];
static uint32_t last_second_ms_2 = 0;
static uint32_t last_ring_ms_2 = 0;
static char last_text_2[9] = "--------";
static char last_name_2[16] = "---------------";
// ---------------------------------

// ---------- Timer 3 State ----------
static char timer_name_3[16] = "Timer 3";
static uint32_t countdown_left_3 = timer_array[2];
static uint32_t last_second_ms_3 = 0;
static uint32_t last_ring_ms_3 = 0;
static char last_text_3[9] = "--------";
static char last_name_3[16] = "---------------";
// ---------------------------------

// ---------- Ring LUT + buffer (no Arduino_Canvas!) ----------
#if USE_RING
static uint16_t ringbuf[RING_SZ * RING_SZ];        // RGB565 bitmap to blit
static uint16_t angleLUT[RING_SZ * RING_SZ];       // angle in [0..65535]
static uint8_t  maskLUT[RING_SZ * RING_SZ];        // 1 = in donut, 0 = outside

static inline int ring_size_px(float scale) {
  int sz = (int)(RING_SZ * scale + 0.5f);
  return sz < 1 ? 1 : sz;
}

static void init_ring_lut(float scale=1.0f) {

  // pick a random color
  int colorPicker = random(0,5);
  switch(colorPicker) {
    case 0:
      GRAD_END = GRAD_BLUE;
      break;
    case 1:
      GRAD_END = GRAD_RED;
      break;
    case 2:
      GRAD_END = GRAD_GREEN;
      break;
    case 3:
      GRAD_END = GRAD_PURPLE;
      break;
    case 4:
      GRAD_END = GRAD_ORANGE;
      break;
  }

  // const float SCALE  = 65535.0f / TWO_PI;
  // const int cx = (int)(RING_SZ * scale) / 2;
  // const int cy = (int)(RING_SZ * scale) / 2;
  // const int ro2 = RING_RO * RING_RO;
  // const int ri2 = RING_RI * RING_RI;
  // for (int y = 0; y < (int)(RING_SZ * scale); y++) {
  //   int dy = y - cy;
  //   int dy2 = dy * dy;
  //   for (int x = 0; x < (int)(RING_SZ * scale); x++) {
  //     int dx = x - cx;
  //     int r2 = dx*dx + dy2;
  //     int idx = y * (int)(RING_SZ * scale) + x;
  //     if (r2 <= ro2 && r2 >= ri2) {
  //       float theta = atan2f((float)dx, (float)(-dy)); // cw angle from 12 o'clock
  //       if (theta < 0) theta += TWO_PI;
  //       angleLUT[idx] = (uint16_t)(theta * SCALE + 0.5f);
  //       maskLUT[idx]  = 1;
  //     } else {
  //       maskLUT[idx]  = 0;
  //     }
  //   }
  // }
  const int SZ  = ring_size_px(scale);
  const float SCALE = 65535.0f / TWO_PI;
  const int cx = SZ / 2;
  const int cy = SZ / 2;

  // *** scale the radii ***
  const int ro = (int)(RING_RO * scale + 0.5f);
  const int ri = (int)(RING_RI * scale + 0.5f);
  const int ro2 = ro * ro;
  const int ri2 = ri * ri;

  for (int y = 0; y < SZ; y++) {
    int dy = y - cy;
    int dy2 = dy * dy;
    for (int x = 0; x < SZ; x++) {
      int dx = x - cx;
      int r2 = dx*dx + dy2;
      int idx = y * SZ + x;
      if (r2 <= ro2 && r2 >= ri2) {
        float theta = atan2f((float)dx, (float)(-dy)); // cw from 12 o'clock
        if (theta < 0) theta += TWO_PI;
        angleLUT[idx] = (uint16_t)(theta * SCALE + 0.5f);
        maskLUT[idx]  = 1;
      } else {
        maskLUT[idx]  = 0;
      }
    }
  }
}

// ----- Cap style toggles -----
enum CapMode : uint8_t {
  CAP_NONE   = 0,
  CAP_LEAD   = 1,   // rounded at the moving end (the advancing edge)
  CAP_TRAIL  = 2,   // rounded at the fixed 12 o'clock end
  CAP_BOTH   = 3    // CAP_LEAD | CAP_TRAIL
};

// Example use in loop(): draw_ring(fracRemaining, CAP_NONE / CAP_LEAD / CAP_BOTH);
// static void draw_ring(float fracRemaining, uint8_t caps, int x=RING_X, int y=RING_Y, uint16_t bg=hex565(0x14215E), float scale=1.0f)
// {
//   if (fracRemaining < 0) fracRemaining = 0;
//   if (fracRemaining > 1) fracRemaining = 1;

//   // Angular threshold on our CW-from-12° LUT
//   const uint16_t threshold = (uint16_t)(fracRemaining * 65535.0f + 0.5f);
//   const uint16_t cut       = (uint16_t)(65535 - threshold); // fill if angle >= cut
//   uint32_t span            = (uint32_t)65535 - (uint32_t)cut;
//   if (span == 0) span = 1;                                   // avoid /0 at time-up

//   // Colors for non-filled areas
//   const uint16_t OUTSIDE = bg;  // page background
//   const uint16_t BG      = hex565(0x44598C);          // donut "empty" color

//   // --- Rounded-cap geometry (on ring midline) ---
//   const int   cx    = (int)(RING_SZ * scale) / 2;
//   const int   cy    = (int)(RING_SZ * scale) / 2;
//   const float thick = float(RING_RO - RING_RI);
//   const float r_mid = 0.5f * (RING_RO + RING_RI);
//   const float cap_r = 0.5f * thick + 0.5f;     // small +0.5 for nicer coverage
//   const float cap_r2 = cap_r * cap_r;

//   // Moving end angle (CW from 12 o'clock) and fixed end angle
//   const float a_lead  = (1.0f - fracRemaining) * TWO_PI;
//   const float a_trail = 0.0f;

//   // Cap centers (0, 1, or 2)
//   float capX[2], capY[2]; uint8_t nCaps = 0;
//   if (caps & CAP_LEAD) {
//     capX[nCaps] = cx + r_mid * sinf(a_lead);
//     capY[nCaps] = cy - r_mid * cosf(a_lead);
//     ++nCaps;
//   }
//   if (caps & CAP_TRAIL) {
//     capX[nCaps] = cx + r_mid * sinf(a_trail);  // = cx
//     capY[nCaps] = cy - r_mid * cosf(a_trail);  // = cy - r_mid
//     ++nCaps;
//   }

//   // Build the bitmap with gradient along the filled arc
//   for (int y = 0; y < (int)(RING_SZ * scale); ++y) {
//     for (int x = 0; x < (int)(RING_SZ * scale); ++x) {
//       const int idx = y * (int)(RING_SZ * scale) + x;

//       if (!maskLUT[idx]) { ringbuf[idx] = OUTSIDE; continue; }

//       const uint16_t a = angleLUT[idx];
//       bool inArc = (a >= cut);
//       uint16_t color = BG;

//       if (inArc) {
//         // position within filled arc: 0 at trail (cut) → 255 at lead (65535)
//         uint8_t t = (uint8_t)(((uint32_t)(a - cut) * 255U) / span);
//         if (invertGradient) t = 255 - t;
//         color = lerp565(GRAD_START, GRAD_END, t);
//       }

//       // Optional rounded caps: force fill + color at the ends
//       if (!inArc && nCaps) {
//         for (uint8_t i = 0; i < nCaps; ++i) {
//           const float dx = (float)x - capX[i];
//           const float dy = (float)y - capY[i];
//           if (dx*dx + dy*dy <= cap_r2) {
//             // Color for cap: lead uses GRAD_END; trail uses GRAD_START
//             color = (i == 0 && (caps & CAP_LEAD)) ? GRAD_START : GRAD_END;
//             inArc = true;
//             break;
//           }
//         }
//       }

//       ringbuf[idx] = color;
//     }
//   }

//   gfx->startWrite();
//   gfx->draw16bitRGBBitmap(x, y, ringbuf, RING_SZ, RING_SZ);
//   gfx->endWrite();
// }
static void draw_ring(float fracRemaining, uint8_t caps,
                      int x=RING_X, int y=RING_Y,
                      uint16_t bg=hex565(0x14215E), float scale=1.0f)
{
  if (fracRemaining < 0) fracRemaining = 0;
  if (fracRemaining > 1) fracRemaining = 1;

  const uint16_t threshold = (uint16_t)(fracRemaining * 65535.0f + 0.5f);
  const uint16_t cut       = (uint16_t)(65535 - threshold);
  uint32_t span            = (uint32_t)65535 - (uint32_t)cut;
  if (span == 0) span = 1;

  const uint16_t OUTSIDE = bg;
  const uint16_t BG      = hex565(0x44598C);

  const int   SZ   = ring_size_px(scale);
  const int   cx   = SZ / 2;
  const int   cy   = SZ / 2;

  // *** scale ring thickness & mid radius ***
  const float ro   = RING_RO * scale;
  const float ri   = RING_RI * scale;
  const float thick= ro - ri;
  const float r_mid= 0.5f * (ro + ri);
  const float cap_r= 0.5f * thick + 0.5f;
  const float cap_r2 = cap_r * cap_r;

  const float a_lead  = (1.0f - fracRemaining) * TWO_PI;
  const float a_trail = 0.0f;

  float capX[2], capY[2]; uint8_t nCaps = 0;
  if (caps & CAP_LEAD) {
    capX[nCaps] = cx + r_mid * sinf(a_lead);
    capY[nCaps] = cy - r_mid * cosf(a_lead);
    ++nCaps;
  }
  if (caps & CAP_TRAIL) {
    capX[nCaps] = cx + r_mid * sinf(a_trail);
    capY[nCaps] = cy - r_mid * cosf(a_trail);
    ++nCaps;
  }

  // *** fill only SZ×SZ, using SZ stride ***
  for (int yy = 0; yy < SZ; ++yy) {
    for (int xx = 0; xx < SZ; ++xx) {
      const int idx = yy * SZ + xx;

      if (!maskLUT[idx]) { ringbuf[idx] = OUTSIDE; continue; }

      const uint16_t a = angleLUT[idx];
      bool inArc = (a >= cut);
      uint16_t color = BG;

      if (inArc) {
        uint8_t t = (uint8_t)(((uint32_t)(a - cut) * 255U) / span);
        if (invertGradient) t = 255 - t;
        color = lerp565(GRAD_START, GRAD_END, t);
      }

      if (!inArc && nCaps) {
        for (uint8_t i = 0; i < nCaps; ++i) {
          const float dx = (float)xx - capX[i];
          const float dy = (float)yy - capY[i];
          if (dx*dx + dy*dy <= cap_r2) {
            color = (i == 0 && (caps & CAP_LEAD)) ? GRAD_START : GRAD_END;
            break;
          }
        }
      }

      ringbuf[idx] = color;
    }
  }

  // *** blit with SZ, not RING_SZ ***
  gfx->startWrite();
  gfx->draw16bitRGBBitmap(x, y, ringbuf, SZ, SZ);
  gfx->endWrite();
}


#endif
// -------------------------------------------------------------

// Helper function to draw the text and ring for a given timer
// Single-mode: the current active timer takes up the whole screen
// Dual-mode: two timers share the screen, one on each side, ring stays the same but text is smaller
// Tri-mode: all three timers share the screen, ring and text all shrink
void renderTimers() {
  // Count active timers
  int active_timers = 0;
  for (int i=0; i<3; i++) {
    if (timer_array[i] > 0) active_timers++;
  }

  gfx->fillScreen(hex565(0x14215E));
  if (active_timers == 1) {
    // Single-mode: full screen for the active timer
    // VU frame once
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);

    // Timer text style: foreground + background (!) so we don't clear big rects
    gfx->setTextSize(8);
    gfx->setTextWrap(false);
    gfx->setTextColor(WHITE, hex565(0x14215E));

    #if USE_RING
      init_ring_lut();   // heavy math done once
      draw_ring(1.0f, CAP_LEAD);
    #endif

    last_second_ms = millis();
    last_ring_ms = millis();
    char hhmmss[9]; 
    char timer_name[16];

    fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss);
    gfx->setCursor(TXT_X, TXT_Y);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_1);
    strcpy(last_name, timer_name);
    gfx->setTextSize(4);
    gfx->setCursor(TXT_X, TXT_Y - 40);
    gfx->print(timer_name);
  } else if (active_timers == 2) {
    // Dual-mode: split screen for the two active timers
    gfx->fillRect(480, 0, 480, 320, hex565(0x2139A4));

    // VU frame once
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);

    gfx->setTextSize(5);
    gfx->setTextWrap(false);
    gfx->setTextColor(WHITE, hex565(0x14215E));

    #if USE_RING
      init_ring_lut();   // heavy math done once
      draw_ring(1.0f, CAP_LEAD, RING_CX - 680, RING_CY - 60);

      draw_ring(1.0f, CAP_LEAD, RING_CX + 480 - 680, RING_CY - 60, hex565(0x2139A4));
    #endif

    last_second_ms = millis();
    last_ring_ms = millis();
    char hhmmss[9]; 
    char timer_name[16];

    fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss);
    gfx->setCursor(TXT_X + 85, TXT_Y - 40 - 40);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_1);
    strcpy(last_name, timer_name);
    gfx->setTextSize(4);
    gfx->setCursor(TXT_X + 85, TXT_Y - 40 - 40 - 40);
    gfx->print(timer_name);

    gfx->setTextSize(5);
    // gfx->setTextColor(WHITE, hex565(0x2139A4));

    fmt_hhmmss(countdown_left_2, hhmmss, hex565(0x2139A4));
    strcpy(last_text_2, hhmmss);
    gfx->setCursor(TXT_X + 480 + 85, TXT_Y - 40 - 40);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_2);
    strcpy(last_name_2, timer_name);
    gfx->setTextSize(4);
    gfx->setCursor(TXT_X + 480 + 85, TXT_Y - 40 - 40 - 40);
    gfx->print(timer_name);
  } else if (active_timers == 3) {
    // Tri-mode: all three timers share the screen
    gfx->fillRect(320, 0, 320, 320, hex565(0x2139A4));
    gfx->fillRect(640, 0, 320, 320, hex565(0x3F56C0));

    // VU frame once
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);

    gfx->setTextSize(4);
    gfx->setTextWrap(false);
    gfx->setTextColor(WHITE, hex565(0x14215E));

    #if USE_RING
      init_ring_lut(0.8f);   // heavy math done once
      draw_ring(1.0f, CAP_LEAD, RING_CX - 650, RING_CY - 30, hex565(0x14215E), 0.8f);

      draw_ring(1.0f, CAP_LEAD, RING_CX + 320 - 650, RING_CY - 30, hex565(0x2139A4), 0.8f);

      draw_ring(1.0f, CAP_LEAD, RING_CX + 640 - 650, RING_CY - 30, hex565(0x3F56C0), 0.8f);
    #endif

    last_second_ms = millis();
    last_ring_ms = millis();
    char hhmmss[9]; 
    char timer_name[16];

    // Timer 1
    fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss);
    gfx->setCursor(TXT_X - 100, TXT_Y - 40 - 40);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_1);
    strcpy(last_name, timer_name);
    gfx->setTextSize(3);
    gfx->setCursor(TXT_X - 100, TXT_Y - 40 - 40 - 40);
    gfx->print(timer_name);


    // Timer 2
    gfx->setTextSize(4);
    // gfx->setTextColor(WHITE, hex565(0x2139A4));

    fmt_hhmmss(countdown_left_2, hhmmss, hex565(0x2139A4));
    strcpy(last_text_2, hhmmss);
    gfx->setCursor(TXT_X + 230, TXT_Y - 40 - 40);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_2);
    strcpy(last_name_2, timer_name);
    gfx->setTextSize(3);
    gfx->setCursor(TXT_X + 230, TXT_Y - 40 - 40 - 40);
    gfx->print(timer_name);

    // Timer 3
    gfx->setTextSize(4);
    // gfx->setTextColor(WHITE, hex565(0x2139A4));

    fmt_hhmmss(countdown_left_3, hhmmss, hex565(0x3F56C0));
    strcpy(last_text_3, hhmmss);
    gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 40 - 40);
    gfx->print(hhmmss);

    // Write the timer name above the time
    sprintf(timer_name, timer_name_3);
    strcpy(last_name_3, timer_name);
    gfx->setTextSize(3);
    gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 40 - 40 - 40);
    gfx->print(timer_name);
  }
}


void setup() {
  Wire.setClock(1000000);
  gfx->begin();
  gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  #if USE_MIC
    // i2s_init();
    Serial.begin(115200);
    // while (!Serial) {
    //   ; // wait for serial port to connect. Needed for native USB port only
    // }

    // // start I2S at 16 kHz with 32-bits per sample
    // if (!I2S.begin(I2S_PHILIPS_MODE, 16000, 32)) {
    //   Serial.println("Failed to initialize I2S!");
    //   while (1); // do nothing
    // }
      i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // or RIGHT depending on SEL
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false
      };
      i2s_pin_config_t pins = { .bck_io_num = PIN_BCLK, .ws_io_num = PIN_WS,
                                .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = PIN_SD };
      i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
      i2s_set_pin(I2S_PORT, &pins);
  #endif

//   // Static background (left only)
//   gfx->fillScreen(hex565(0x14215E));
// //   const uint16_t cols[] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE };
// //   int bh = gfx->height() / 7;
// //   for (int i = 0; i < 7; i++) gfx->fillRect(0, i*bh, UI_RIGHT_Y, bh, cols[i]);
// //   draw_grid_left(DARKGREY, 16, 16);

// //   // Right panel once (we won't clear it every frame)
// //   gfx->fillRect(UI_RIGHT_Y, 0, UI_RIGHT_H, gfx->height(), BLACK);


//   // Figure out how many timers need to be rendered
//   int active_timers = 0;
//   for (int i = 0; i < 3; i++) {
//     if (timer_array[i] > 0) active_timers++;
//   }

//   if (active_timers == 3) {
//     // draw a 320x320 rect in the center and another in the right
//     gfx->fillRect(320, 0, 320, 320, hex565(0x2139A4));
//     gfx->fillRect(640, 0, 320, 320, hex565(0x3F56C0));

//   } else if (active_timers == 2) {
//     gfx->fillRect(480, 0, 480, 320, hex565(0x2139A4));
    
    
//   } else {

//   }

//   // VU frame once
//   gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);

//   // Timer text style: foreground + background (!) so we don't clear big rects
//   gfx->setTextSize(8);
//   gfx->setTextWrap(false);
//   gfx->setTextColor(WHITE, hex565(0x14215E));

//   #if USE_RING
//     init_ring_lut();   // heavy math done once
//   #endif



//   // Initial timer draw
//   last_second_ms = millis();
//   char hhmmss[9]; 
//   char timer_name[16];

//   fmt_hhmmss(countdown_left, hhmmss);
//   strcpy(last_text, hhmmss);
//   gfx->setCursor(TXT_X, TXT_Y);
//   gfx->print(hhmmss);

//   // Write the timer name above the time
//   sprintf(timer_name, "Countdown Timer");
//   strcpy(last_name, timer_name);
//   gfx->setTextSize(4);
//   gfx->setCursor(TXT_X, TXT_Y - 40);
//   gfx->print(timer_name);

  renderTimers();
}

void loop() {
  uint32_t now = millis();

  // Count active timers
  int active_timers = 0;
  for (int i=0; i<3; i++) {
    if (timer_array[i] > 0) active_timers++;
  }

// --- Update countdown once per second ---
if (now - last_second_ms >= 1000) { // if a second has passed
  last_second_ms += 1000;

  char hhmmss1[9];
  char hhmmss2[9];
  char hhmmss3[9];
  // float frac1 = 1.0f;
  // float frac2 = 1.0f;
  // float frac3 = 1.0f;

  // float sub;

  // --- Animate ring ~30 FPS, tied to whole countdown ---
  // #if USE_RING
  // static uint32_t last_ring_ms = 0;
  // if (now - last_ring_ms >= 100) {                 // 33 --> ~30 FPS
  //   last_ring_ms = now;

  //   // fractional seconds within the current second
  //   sub = (float)(now - last_second_ms) / 1000.0f;
  //   if (sub < 0) sub = 0; if (sub > 1) sub = 1;
  // }
  // #endif


  if (active_timers >= 1) {
    countdown_left = (countdown_left > 0) ? (countdown_left - 1) : COUNTDOWN_SECONDS_1;
    fmt_hhmmss(countdown_left, hhmmss1, hex565(0x14215E));

    // #if USE_RING
    //     // exact seconds remaining, including sub-second
    //   float remainingExact = (float)countdown_left + (1.0f - sub);
    //   if (remainingExact < 0) remainingExact = 0;

    //   // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    //   frac1 = remainingExact / (float)COUNTDOWN_SECONDS_1;
    // #endif
  }
  if (active_timers >= 2) {
    countdown_left_2 = (countdown_left_2 > 0) ? (countdown_left_2 - 1) : COUNTDOWN_SECONDS_2;
    fmt_hhmmss(countdown_left_2, hhmmss2, hex565(0x2139A4));

    // #if USE_RING
    //     // exact seconds remaining, including sub-second
    //   float remainingExact2 = (float)countdown_left_2 + (1.0f - sub);
    //   if (remainingExact2 < 0) remainingExact2 = 0;

    //   // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    //   frac2 = remainingExact2 / (float)COUNTDOWN_SECONDS_2;
    // #endif
  }
  if (active_timers >= 3) {
    countdown_left_3 = (countdown_left_3 > 0) ? (countdown_left_3 - 1) : COUNTDOWN_SECONDS_3;
    fmt_hhmmss(countdown_left_3, hhmmss3, hex565(0x3F56C0));

    // #if USE_RING
    //     // exact seconds remaining, including sub-second
    //   float remainingExact3 = (float)countdown_left_3 + (1.0f - sub);
    //   if (remainingExact3 < 0) remainingExact3 = 0;

    //   // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    //   frac3 = remainingExact3 / (float)COUNTDOWN_SECONDS_3;
    // #endif
  } 


  if (active_timers == 1) {
      if (strcmp(hhmmss1, last_text) != 0) {
      strcpy(last_text, hhmmss1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(8);
      gfx->setCursor(TXT_X, TXT_Y);
      gfx->print(hhmmss1);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_1, last_name) != 0) {
      strcpy(last_name, timer_name_1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X - 60, TXT_Y - 40);
      gfx->print(timer_name_1);
    }

    // #if USE_RING
    //   draw_ring(frac1, CAP_LEAD);
    // #endif
  } else if (active_timers == 2) {
    // #if USE_RING
    //   draw_ring(frac1, CAP_LEAD, RING_CX - 680, RING_CY - 60);

    //   draw_ring(frac2, CAP_LEAD, RING_CX + 480 - 680, RING_CY - 60, hex565(0x2139A4));
    // #endif
    if (strcmp(hhmmss1, last_text) != 0) { // TODO: wrong background color for some reason
      strcpy(last_text, hhmmss1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(5);
      gfx->setCursor(TXT_X + 85, TXT_Y - 40 - 40);
      gfx->print(hhmmss1);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_1, last_name) != 0) {
      strcpy(last_name, timer_name_1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X + 85, TXT_Y - 40 - 40 - 40);
      gfx->print(timer_name_1);
    }

    if (strcmp(hhmmss2, last_text_2) != 0) {
      strcpy(last_text_2, hhmmss2);
      gfx->setTextColor(WHITE, hex565(0x2139A4));
      gfx->setTextSize(5);
      gfx->setCursor(TXT_X + 480 + 85, TXT_Y - 40 - 40);
      gfx->print(hhmmss2);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_2, last_name_2) != 0) {
      strcpy(last_name_2, timer_name_2);
      gfx->setTextColor(WHITE, hex565(0x2139A4));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X + 480 + 85, TXT_Y - 40 - 40 - 40);
      gfx->print(timer_name_2);
    }
  } else if (active_timers == 3) {
    // #if USE_RING
    //   draw_ring(frac1, CAP_LEAD, RING_CX - 650, RING_CY - 30, hex565(0x14215E), 0.8f);

    //   draw_ring(frac2, CAP_LEAD, RING_CX + 320 - 650, RING_CY - 30, hex565(0x2139A4), 0.8f);

    //   draw_ring(frac3, CAP_LEAD, RING_CX + 640 - 650, RING_CY - 30, hex565(0x3F56C0), 0.8f);
    // #endif
    if (strcmp(hhmmss1, last_text) != 0) {
      strcpy(last_text, hhmmss1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X - 100, TXT_Y - 40 - 40);
      gfx->print(hhmmss1);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_1, last_name) != 0) {
      strcpy(last_name, timer_name_1);
      gfx->setTextColor(WHITE, hex565(0x14215E));
      gfx->setTextSize(3);
      gfx->setCursor(TXT_X - 100, TXT_Y - 40 - 40 - 40);
      gfx->print(timer_name_1);
    }

    if (strcmp(hhmmss2, last_text_2) != 0) {
      strcpy(last_text_2, hhmmss2);
      gfx->setTextColor(WHITE, hex565(0x2139A4));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X + 230, TXT_Y - 40 - 40);
      gfx->print(hhmmss2);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_2, last_name_2) != 0) {
      strcpy(last_name_2, timer_name_2);
      gfx->setTextColor(WHITE, hex565(0x2139A4));
      gfx->setTextSize(3);
      gfx->setCursor(TXT_X + 230, TXT_Y - 40 - 40 - 40);
      gfx->print(timer_name_2);
    }

    if (strcmp(hhmmss3, last_text_3) != 0) {
      strcpy(last_text_3, hhmmss3);
      gfx->setTextColor(WHITE, hex565(0x3F56C0));
      gfx->setTextSize(4);
      gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 40 - 40);
      gfx->print(hhmmss3);  // white text with BLACK bg; tiny overwrite only
    }

    if (strcmp(timer_name_3, last_name_3) != 0) {
      strcpy(last_name_3, timer_name_3);
      gfx->setTextColor(WHITE, hex565(0x3F56C0));
      gfx->setTextSize(3);
      gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 40 - 40 - 40);
      gfx->print(timer_name_3);
    }
  }

//   // Update the text once per second
//   char hhmmss[9]; fmt_hhmmss(countdown_left, hhmmss);
//   char timer_name[16]; sprintf(timer_name, "Countdown Timer");

//   if (strcmp(hhmmss, last_text) != 0) {
//     strcpy(last_text, hhmmss);
//     gfx->setTextSize(8);
//     gfx->setCursor(TXT_X, TXT_Y);
//     gfx->print(hhmmss);  // white text with BLACK bg; tiny overwrite only
//   }

//   if (strcmp(timer_name, last_name) != 0) {
//     strcpy(last_name, timer_name);
//     gfx->setTextSize(4);
//     gfx->setCursor(TXT_X - 60, TXT_Y - 40);
//     gfx->print(timer_name);
//   }
}

// Separately update rings to allow smoother animation
const uint32_t FRAME_DT = 67;
if (now - last_ring_ms >= FRAME_DT) {
  do { last_ring_ms +=  }

  #if USE_RING
  last_ring_ms = now;
  float frac1 = 1.0f;
  float frac2 = 1.0f;
  float frac3 = 1.0f;

  float sub;

  // fractional seconds within the current second
  sub = (float)(now - last_ring_ms) / 1000.0f;
  if (sub < 0) sub = 0; if (sub > 1) sub = 1;

  if (active_timers >= 1) {
    float remainingExact = (float)countdown_left + (0.067f - sub);
    if (remainingExact < 0) remainingExact = 0;

    // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    frac1 = remainingExact / (float)COUNTDOWN_SECONDS_1;
  }
  if (active_timers >= 2) {
    float remainingExact2 = (float)countdown_left_2 + (0.067f - sub);
    if (remainingExact2 < 0) remainingExact2 = 0;

    // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    frac2 = remainingExact2 / (float)COUNTDOWN_SECONDS_2;
  }
  if (active_timers >= 3) {
    float remainingExact3 = (float)countdown_left_3 + (0.067f - sub);
    if (remainingExact3 < 0) remainingExact3 = 0;

    // fraction of total time remaining (1 → 0 over the entire 10 minutes)
    frac3 = remainingExact3 / (float)COUNTDOWN_SECONDS_3;
  }

  if (active_timers == 1) {
    draw_ring(frac1, CAP_LEAD);
  } else if (active_timers == 2) {
    draw_ring(frac1, CAP_LEAD, RING_CX - 680, RING_CY - 60);
    draw_ring(frac2, CAP_LEAD, RING_CX + 480 - 680, RING_CY - 60, hex565(0x2139A4));
  } else {
    draw_ring(frac1, CAP_LEAD, RING_CX - 650, RING_CY - 30, hex565(0x14215E), 0.8f);
    draw_ring(frac2, CAP_LEAD, RING_CX + 320 - 650, RING_CY - 30, hex565(0x2139A4), 0.8f);
    draw_ring(frac3, CAP_LEAD, RING_CX + 640 - 650, RING_CY - 30, hex565(0x3F56C0), 0.8f);
  }

  #endif
}

# if USE_MIC
  // int sample = I2S.read();

  // if ((sample == 0) || (sample == -1) ) {
  //   return;
  // }
  // // convert to 18 bit signed
  // sample >>= 14; 

  // // if it's non-zero print value to serial
  // Serial.println(sample);
    int32_t sampleBuf[256];
    size_t readBytes = 0;
    i2s_read(I2S_PORT, sampleBuf, sizeof(sampleBuf), &readBytes, portMAX_DELAY);

    Serial.print("Read bytes: ");
    // output the numbers to serial
    for (size_t i = 0; i < readBytes / sizeof(int32_t); i++) {
      Serial.println(sampleBuf[i]);
    }
# endif

// --- Animate ring ~30 FPS, tied to whole countdown ---
// #if USE_RING
// static uint32_t last_ring_ms = 0;
// if (now - last_ring_ms >= 100) {                 // 33 --> ~30 FPS
//   last_ring_ms = now;

//   // fractional seconds within the current second
//   float sub = (float)(now - last_second_ms) / 1000.0f;
//   if (sub < 0) sub = 0; if (sub > 1) sub = 1;

//   // exact seconds remaining, including sub-second
//   float remainingExact = (float)countdown_left + (1.0f - sub);
//   if (remainingExact < 0) remainingExact = 0;

//   // fraction of total time remaining (1 → 0 over the entire 10 minutes)
//   float fracRemaining = remainingExact / (float)COUNTDOWN_SECONDS;

//   draw_ring(fracRemaining, CAP_LEAD);
// }
// #endif


  delay(2);
}

