/*
 * Timer Device - Production Firmware
 * 
 * Hardware:
 *   - Qualia ESP32-S3 RGB666
 *   - HD458002C40 4.58" 320x960 RGB TTL TFT Display
 *   - SPH0645 I2S MEMS Microphone
 *   - Adafruit STEMMA Speaker (via JST on A0)
 *   - Nicla Voice (BLE communication for voice commands)
 * 
 * Features:
 *   - Up to 3 concurrent timers with visual countdown rings
 *   - Animated activity GIFs for the timer art
 *   - BLE communication with Nicla Voice for voice commands
 *   - Sound effects for feedback (bootup, confirm, cancel, alarm)
 *   - Voice commands: Set, Cancel, Add, Minus, Stop
 */
#include <stdint.h>

// Arduino's sketch preprocessor auto-inserts function prototypes near the top of
// the file. Forward-declare custom types used in function signatures so those
// auto-generated prototypes compile cleanly.
struct gd_GIF;
typedef struct gd_GIF gd_GIF;
enum TimerThemeId : uint8_t;
struct TimerTheme;
struct Timer;
struct ThemeGifState;
struct PanelLayout;

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPIFFS.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <vector>
#include <string>
#include "command_protocol.h"
#include "font/TimerUiFont.h"
#include <NimBLEDevice.h>

// ======================= FEATURE FLAGS =======================
#define USE_MIC       0   // Disable local mic (using Nicla Voice instead)
#define USE_RING      1   // Enable ring animation
#define USE_GIFS      1   // Re-enable GIFs; rely on auto-flush framebuffer updates
#define USE_SPEAKER   1   // Enable speaker output
#define USE_BLE       1   // Enable BLE for Nicla Voice communication

// If your speaker is driven by an analog/PWM input (common on STEMMA speaker amps),
// MP3->I2S output will sound awful. Default to PWM-based SFX.
#define USE_MP3_SFX   0   // 0 = PWM beep SFX (recommended), 1 = MP3 over I2S

// Filesystem used for activity GIF assets.
#define USE_LITTLEFS      1
#define FS_FORMAT_ON_FAIL 0

// ======================= AUDIO LEVELS =======================
// MP3 SFX on small Class-D amps can clip easily. Start conservative.
static float g_sfx_gain = 0.12f;  // 0.0 .. 1.0

// PWM loudness control (only used when USE_MP3_SFX == 0)
// With 8-bit resolution, duty is 0..255. Keep conservative to avoid painful output.
static uint8_t g_pwm_duty = 32;

// ======================= DEMO MODE =======================
// Set to true to simulate Nicla Voice commands for demonstration
// When false, operates normally waiting for BLE commands from Nicla Voice
static const bool INDEPENDENT_DEMO = true;

// Demo command structure - defines what command to run and when
struct DemoCommand {
  uint32_t trigger_ms;      // Time after boot to trigger (milliseconds)
  const char* command;      // Raw command string (same format as BLE)
  bool executed;            // Has this been executed?
};

// Demo command queue - edit this array to customize the demo sequence
// Commands trigger at the specified time after boot
// Format: "CMD:SET,NAME:TimerName,DURATION:seconds"
//         "CMD:CANCEL,NAME:TimerName"
//         "CMD:ADD,NAME:TimerName,DURATION:seconds"
//         "CMD:MINUS,NAME:TimerName,DURATION:seconds"
//         "CMD:STOP"
static DemoCommand demo_command_queue[] = {
  // Start with no timers, then add them one by one
  { 3000,  "CMD:SET,NAME:Baking,DURATION:180", false },     // 3 sec: Create 3-minute "Baking" timer
  { 8000,  "CMD:SET,NAME:Break,DURATION:120", false },      // 8 sec: Create 2-minute "Break" timer
  { 15000, "CMD:ADD,NAME:Baking,DURATION:60", false },      // 15 sec: Add 1 minute to Baking
  { 20000, "CMD:SET,NAME:Homework,DURATION:90", false },    // 20 sec: Create 90-second "Homework" timer (3rd)
  { 30000, "CMD:MINUS,NAME:Break,DURATION:30", false },     // 30 sec: Subtract 30 sec from Break
  { 45000, "CMD:CANCEL,NAME:Homework", false },             // 45 sec: Cancel Homework timer
  // Timer will ring when it hits 0, then:
  // { 200000, "CMD:STOP", false },                         // Uncomment to auto-stop alarms
};
static const int DEMO_COMMAND_COUNT = sizeof(demo_command_queue) / sizeof(demo_command_queue[0]);
static uint32_t demo_start_ms = 0;

// ======================= I2S SPEAKER CONFIG =======================
#if USE_SPEAKER
  #include <esp32-hal-ledc.h>

  // STEMMA speaker JST (SIG is A0). We'll use LEDC PWM for SFX + alarm tones.
  static const int AUDIO_PIN = A0;

  #if USE_MP3_SFX
    #include <AudioFileSourcePROGMEM.h>
    #include <AudioGeneratorMP3.h>
    #include <AudioOutputI2S.h>
    #include "sounds/sfx_mp3_data.h"

    #define I2S_SPK_BCLK  SCK
    #define I2S_SPK_LRC   MOSI
    #define I2S_SPK_DOUT  A0

    static AudioGeneratorMP3* g_mp3 = nullptr;
    static AudioFileSourcePROGMEM* g_src = nullptr;
    static AudioOutputI2S* g_out = nullptr;
  #endif
#endif

// ======================= DISPLAY CONFIG =======================
#define PCLK_HZ         12000000
#define PCLK_ACTIVE_NEG 1
#define H_FRONT   24
#define H_PULSE    4
#define H_BACK    64
#define V_FRONT   12
#define V_PULSE    2
#define V_BACK    20

// ======================= UI LAYOUT =======================
const int VU_W = 44;
const int UI_RIGHT_H = 140;
const int UI_RIGHT_Y = 320 - UI_RIGHT_H;
const int BITMAP_X_ALIGN_FIX = 80;

const int RING_SZ = 192;
const int RING_RO = 96;
const int RING_RI = 72;
const int RING_CY = 160;
const int RING_CX = 650;
const int RING_X  = RING_CX - RING_SZ/2;
const int RING_Y  = RING_CY - RING_SZ/2;

const int TXT_Y = UI_RIGHT_Y - 32;
const int TXT_X = RING_CX - RING_RO - 412;

// ======================= DISPLAY OBJECTS =======================
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
static inline uint16_t lerp565(uint16_t c0, uint16_t c1, uint8_t t) {
  uint16_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  uint16_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint16_t r = (uint16_t)((r0 * (255 - t) + r1 * t) / 255);
  uint16_t g = (uint16_t)((g0 * (255 - t) + g1 * t) / 255);
  uint16_t b = (uint16_t)((b0 * (255 - t) + b1 * t) / 255);
  return (r << 11) | (g << 5) | b;
}

enum TimerThemeId : uint8_t {
  THEME_DEFAULT = 0,
  THEME_BREAK = 1,
  THEME_HOMEWORK = 2,
  THEME_BAKING = 3,
  THEME_EXERCISE = 4,
  THEME_COUNT = 5
};

struct TimerTheme {
  uint16_t bg;
  uint16_t ring_start;
  uint16_t ring_end;
  uint16_t ring_empty;
  uint16_t text;
  uint16_t art_primary;
  uint16_t art_secondary;
  uint16_t art_tertiary;
  uint16_t art_shadow;
};

static const uint16_t COLOR_IDLE_BG = hex565(0x181a20);
static const uint16_t COLOR_IDLE_SUB = hex565(0x99a2b1);
static const uint16_t COLOR_IDLE_BORDER = hex565(0xffffff);
static const uint16_t COLOR_IDLE_BORDER_SOFT = hex565(0xc7d1e2);
static const uint16_t COLOR_ALERT_RED = hex565(0xff738d);
static const uint16_t COLOR_ALERT_ORANGE = hex565(0xff9c5a);
static const uint16_t COLOR_ALERT_START = hex565(0xffeef2);
static const uint16_t COLOR_TEXT_WHITE = hex565(0xffffff);

// Edit activity colors here.
// Each entry controls the panel background plus the ring gradient/empty ring tint.
static const TimerTheme THEME_STYLES[THEME_COUNT] = {
  {
    hex565(0x46637f),
    hex565(0xf2f5f8),
    hex565(0xb5c4d9),
    hex565(0x90a1b8),
    COLOR_TEXT_WHITE,
    hex565(0xd8e6f2),
    hex565(0xa7bed3),
    hex565(0xffffff),
    hex565(0x33495f)
  },
  {
    hex565(0x4f403f),
    hex565(0xf4f4f4),
    hex565(0xd2c9c6),
    hex565(0x9b8d8a),
    COLOR_TEXT_WHITE,
    hex565(0xe9e5e2),
    hex565(0xc8c8cb),
    hex565(0x6c4320),
    hex565(0x7d6f6e)
  },
  {
    hex565(0x496786),
    hex565(0xf2f5f8),
    hex565(0xafbdd5),
    hex565(0x7e94b1),
    COLOR_TEXT_WHITE,
    hex565(0xdce7f1),
    hex565(0xb6c9d8),
    hex565(0xe8f0fa),
    hex565(0x5b7591)
  },
  {
    hex565(0x86465b),
    hex565(0xf6e7eb),
    hex565(0xf79eb2),
    hex565(0xb27186),
    COLOR_TEXT_WHITE,
    hex565(0xff9274),
    hex565(0xffd8ad),
    hex565(0x6ec8c8),
    hex565(0x6e3a4d)
  },
  {
    hex565(0x28656c),
    hex565(0xebf7f4),
    hex565(0x63d6c7),
    hex565(0x5a9998),
    COLOR_TEXT_WHITE,
    hex565(0xc5e7df),
    hex565(0xa6d4cb),
    hex565(0xe5f7f3),
    hex565(0x1e4f57)
  }
};

static bool invertGradient = false;

static void point_on_rect_perimeter(int left, int top, int right, int bottom,
                                    int dist, int* x, int* y) {
  int width = right - left;
  int height = bottom - top;
  int perimeter = (width + height) * 2;
  if (perimeter <= 0) {
    *x = left;
    *y = top;
    return;
  }

  dist %= perimeter;
  if (dist < 0) dist += perimeter;

  if (dist < width) {
    *x = left + dist;
    *y = top;
    return;
  }
  dist -= width;

  if (dist < height) {
    *x = right;
    *y = top + dist;
    return;
  }
  dist -= height;

  if (dist < width) {
    *x = right - dist;
    *y = bottom;
    return;
  }
  dist -= width;

  *x = left;
  *y = bottom - dist;
}

static void draw_idle_glow_pixel(int x, int y, uint8_t strength) {
  if (x < 0 || x >= 960 || y < 0 || y >= 320) return;

  uint16_t inner = lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER, strength);
  gfx->drawPixel(x, y, inner);

  uint16_t mid = lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER, (uint8_t)(strength * 3 / 5));
  if (x > 0) gfx->drawPixel(x - 1, y, mid);
  if (x < 959) gfx->drawPixel(x + 1, y, mid);
  if (y > 0) gfx->drawPixel(x, y - 1, mid);
  if (y < 319) gfx->drawPixel(x, y + 1, mid);

  uint16_t outer = lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER_SOFT, strength / 2);
  if (x > 1) gfx->drawPixel(x - 2, y, outer);
  if (x < 958) gfx->drawPixel(x + 2, y, outer);
  if (y > 1) gfx->drawPixel(x, y - 2, outer);
  if (y < 318) gfx->drawPixel(x, y + 2, outer);

  uint16_t far = lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER_SOFT, strength / 4);
  if (x > 2) gfx->drawPixel(x - 3, y, far);
  if (x < 957) gfx->drawPixel(x + 3, y, far);
  if (y > 2) gfx->drawPixel(x, y - 3, far);
  if (y < 317) gfx->drawPixel(x, y + 3, far);
}

static void draw_idle_border(uint32_t now) {
  const int left = 24;
  const int top = 20;
  const int right = 960 - 25;
  const int bottom = 320 - 21;

  gfx->drawRect(left - 3, top - 3, right - left + 7, bottom - top + 7,
                lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER_SOFT, 36));
  gfx->drawRect(left - 2, top - 2, right - left + 5, bottom - top + 5,
                lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER_SOFT, 58));
  gfx->drawRect(left - 1, top - 1, right - left + 3, bottom - top + 3,
                lerp565(COLOR_IDLE_BG, COLOR_IDLE_BORDER, 80));
  gfx->drawRect(left, top, right - left + 1, bottom - top + 1,
                COLOR_IDLE_BORDER);

  int width = right - left;
  int height = bottom - top;
  int perimeter = (width + height) * 2;
  int lead = (int)((now / 7U) % (uint32_t)perimeter);
  const int trail = 220;

  for (int i = 0; i < trail; i++) {
    int x, y;
    point_on_rect_perimeter(left, top, right, bottom, lead - i, &x, &y);
    uint8_t strength = (uint8_t)(255 - ((uint32_t)i * 235U) / (uint32_t)trail);
    draw_idle_glow_pixel(x, y, strength);
  }
}

static void clear_idle_border_band() {
  const int left = 24;
  const int top = 20;
  const int right = 960 - 25;
  const int bottom = 320 - 21;
  const int band = 7;

  gfx->fillRect(left - band, top - band, right - left + 1 + band * 2, band * 2 + 1, COLOR_IDLE_BG);
  gfx->fillRect(left - band, bottom - band, right - left + 1 + band * 2, band * 2 + 1, COLOR_IDLE_BG);
  gfx->fillRect(left - band, top + band, band * 2 + 1, bottom - top - band * 2 + 1, COLOR_IDLE_BG);
  gfx->fillRect(right - band, top + band, band * 2 + 1, bottom - top - band * 2 + 1, COLOR_IDLE_BG);
}

// ======================= TIMER STATE =======================
#define MAX_TIMERS 3
#define ALARM_DURATION_MS 90000  // 90 seconds auto-shutoff

struct Timer {
  char name[16];
  uint32_t total_seconds;      // Original duration
  uint32_t seconds_left;       // Current countdown
  bool active;
  bool ringing;
  uint32_t ring_start_ms;      // When alarm started
  uint8_t theme_id;
};

static Timer timers[MAX_TIMERS];
static int active_timer_count = 0;

static bool name_equals_ignore_case(const char* lhs, const char* rhs) {
  while (*lhs && *rhs) {
    if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
      return false;
    }
    ++lhs;
    ++rhs;
  }
  return *lhs == '\0' && *rhs == '\0';
}

static TimerThemeId detect_theme_id(const char* name) {
  if (name_equals_ignore_case(name, "Break")) return THEME_BREAK;
  if (name_equals_ignore_case(name, "Homework")) return THEME_HOMEWORK;
  if (name_equals_ignore_case(name, "Baking") || name_equals_ignore_case(name, "Cooking")) return THEME_BAKING;
  if (name_equals_ignore_case(name, "Exercise") || name_equals_ignore_case(name, "Workout")) return THEME_EXERCISE;
  return THEME_DEFAULT;
}

static TimerTheme theme_from_id(uint8_t theme_id) {
  if (theme_id < THEME_COUNT) return THEME_STYLES[theme_id];
  return THEME_STYLES[THEME_DEFAULT];
}

static TimerTheme resolved_theme_for_timer(const Timer& timer) {
  return theme_from_id(timer.theme_id);
}

// ======================= RING BUFFER =======================
#if USE_RING
static uint16_t ringbuf[RING_SZ * RING_SZ];
static uint16_t angleLUT[RING_SZ * RING_SZ];
static uint8_t  maskLUT[RING_SZ * RING_SZ];
static float current_ring_lut_scale = -1.0f;

static inline int ring_size_px(float scale) {
  int sz = (int)(RING_SZ * scale + 0.5f);
  return sz < 1 ? 1 : sz;
}

static void init_ring_lut(float scale = 1.0f) {
  const int SZ = ring_size_px(scale);
  const float SCALE = 65535.0f / TWO_PI;
  const int cx = SZ / 2;
  const int cy = SZ / 2;

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
        float theta = atan2f((float)dx, (float)(-dy));
        if (theta < 0) theta += TWO_PI;
        angleLUT[idx] = (uint16_t)(theta * SCALE + 0.5f);
        maskLUT[idx] = 1;
      } else {
        maskLUT[idx] = 0;
      }
    }
  }
}

static void ensure_ring_lut(float scale = 1.0f) {
  if (current_ring_lut_scale != scale) {
    init_ring_lut(scale);
    current_ring_lut_scale = scale;
  }
}

enum CapMode : uint8_t {
  CAP_NONE  = 0,
  CAP_LEAD  = 1,
  CAP_TRAIL = 2,
  CAP_BOTH  = 3
};

static void draw_ring(float fracRemaining, uint8_t caps,
                      uint16_t grad_start, uint16_t grad_end, uint16_t empty_ring,
                      int x = RING_X, int y = RING_Y,
                      uint16_t bg = COLOR_IDLE_BG, float scale = 1.0f) {
  if (fracRemaining < 0) fracRemaining = 0;
  if (fracRemaining > 1) fracRemaining = 1;

  const uint16_t threshold = (uint16_t)(fracRemaining * 65535.0f + 0.5f);
  const uint16_t cut = (uint16_t)(65535 - threshold);
  uint32_t span = (uint32_t)65535 - (uint32_t)cut;
  if (span == 0) span = 1;

  const uint16_t OUTSIDE = bg;

  const int SZ = ring_size_px(scale);
  const int cx = SZ / 2;
  const int cy = SZ / 2;

  const float ro = RING_RO * scale;
  const float ri = RING_RI * scale;
  const float thick = ro - ri;
  const float r_mid = 0.5f * (ro + ri);
  const float cap_r = 0.5f * thick + 0.5f;
  const float cap_r2 = cap_r * cap_r;

  const float a_lead = (1.0f - fracRemaining) * TWO_PI;
  const float a_trail = 0.0f;

  float capX[2], capY[2];
  uint8_t nCaps = 0;
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

  for (int yy = 0; yy < SZ; ++yy) {
    for (int xx = 0; xx < SZ; ++xx) {
      const int idx = yy * SZ + xx;

      if (!maskLUT[idx]) {
        ringbuf[idx] = OUTSIDE;
        continue;
      }

      const uint16_t a = angleLUT[idx];
      bool inArc = (a >= cut);
      uint16_t color = empty_ring;

      if (inArc) {
        uint8_t t = (uint8_t)(((uint32_t)(a - cut) * 255U) / span);
        if (invertGradient) t = 255 - t;
        color = lerp565(grad_start, grad_end, t);
      }

      if (!inArc && nCaps) {
        for (uint8_t i = 0; i < nCaps; ++i) {
          const float dx = (float)xx - capX[i];
          const float dy = (float)yy - capY[i];
          if (dx*dx + dy*dy <= cap_r2) {
            color = (i == 0 && (caps & CAP_LEAD)) ? grad_start : grad_end;
            inArc = true;
            break;
          }
        }
      }

      ringbuf[idx] = color;
    }
  }

  gfx->startWrite();
  gfx->draw16bitRGBBitmap(x - BITMAP_X_ALIGN_FIX, y, ringbuf, SZ, SZ);
  gfx->endWrite();
}
#endif

// ======================= GIF RUNTIME =======================
#if USE_GIFS
#if defined(ESP32)
  #include <esp_heap_caps.h>
  static void *gif_alloc(size_t bytes) {
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return malloc(bytes);
  }
#else
  static void *gif_alloc(size_t bytes) { return malloc(bytes); }
#endif

#ifndef MIN
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif

#ifndef MAX
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif

static const char *GIF_ASSET_PATHS[THEME_COUNT] = {
  nullptr,
  "/assets/coffee.gif",
  "/assets/books.gif",
  "/assets/mixy.gif",
  "/assets/dumbell.gif"
};

static const uint32_t MIN_GIF_FRAME_DELAY_MS = 20;

typedef struct gd_Palette {
  int16_t len;
  uint16_t colors[256];
} gd_Palette;

typedef struct gd_GCE {
  uint16_t delay;
  uint8_t tindex;
  uint8_t disposal;
  uint8_t input;
  uint8_t transparency;
} gd_GCE;

typedef struct gd_Entry {
  int32_t len;
  uint16_t prefix;
  uint8_t suffix;
} gd_Entry;

typedef struct gd_Table {
  int16_t bulk;
  int16_t nentries;
  gd_Entry *entries;
} gd_Table;

struct gd_GIF {
  File *fd;
  off_t anim_start;
  uint16_t width, height;
  uint16_t depth;
  uint16_t loop_count;
  gd_GCE gce;
  gd_Palette *palette;
  gd_Palette lct, gct;
  void (*plain_text)(
    struct gd_GIF *gif, uint16_t tx, uint16_t ty,
    uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
    uint8_t fg, uint8_t bg);
  void (*comment)(struct gd_GIF *gif);
  void (*application)(struct gd_GIF *gif, char id[8], char auth[3]);
  uint16_t fx, fy, fw, fh;
  uint8_t bgindex;
  gd_Table *table;
  bool read_first_frame;
};

class GifClass {
public:
  gd_GIF *gd_open_gif(File *fd) {
    uint8_t sigver[3];
    uint16_t width, height, depth;
    uint8_t fdsz, bgidx, aspect;
    int16_t gct_sz;
    gd_GIF *gif;

    gif_buf_last_idx = GIF_BUF_SIZE;
    gif_buf_idx = gif_buf_last_idx;
    file_pos = 0;

    gif_buf_read(fd, sigver, 3);
    if (memcmp(sigver, "GIF", 3) != 0) return nullptr;

    gif_buf_read(fd, sigver, 3);
    if (memcmp(sigver, "89a", 3) != 0) return nullptr;

    width = gif_buf_read16(fd);
    height = gif_buf_read16(fd);

    gif_buf_read(fd, &fdsz, 1);
    if (!(fdsz & 0x80)) return nullptr;

    depth = ((fdsz >> 4) & 7) + 1;
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    gif_buf_read(fd, &bgidx, 1);
    gif_buf_read(fd, &aspect, 1);

    gif = (gd_GIF *)calloc(1, sizeof(*gif));
    if (!gif) return nullptr;

    gif->fd = fd;
    gif->width = width;
    gif->height = height;
    gif->depth = depth;

    read_palette(fd, &gif->gct, gct_sz);
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    gif->anim_start = file_pos;

    gif->table = new_table();
    gif->read_first_frame = false;
    if (!gif->table) {
      free(gif);
      return nullptr;
    }
    return gif;
  }

  int32_t gd_get_frame(gd_GIF *gif, uint8_t *frame) {
    char sep;
    gif->gce = {};

    while (1) {
      gif_buf_read(gif->fd, (uint8_t *)&sep, 1);
      if (sep == 0) gif_buf_read(gif->fd, (uint8_t *)&sep, 1);
      if (sep == ',') break;
      if (sep == ';') return 0;
      if (sep == '!') {
        read_ext(gif);
      } else {
        return -1;
      }
    }

    if (read_image(gif, frame) == -1) return -1;
    return 1;
  }

  void gd_rewind(gd_GIF *gif) {
    gif->fd->seek(gif->anim_start, SeekSet);
    file_pos = gif->anim_start;
    gif_buf_idx = gif_buf_last_idx;
  }

  void gd_close_gif(gd_GIF *gif) {
    if (!gif) return;
    if (gif->fd) gif->fd->close();
    if (gif->table) free(gif->table);
    free(gif);
  }

private:
  static const int GIF_BUF_SIZE = 1024;

  bool gif_buf_seek(File *fd, int16_t len) {
    if (len > (gif_buf_last_idx - gif_buf_idx)) {
      fd->seek(file_pos + len - (gif_buf_last_idx - gif_buf_idx), SeekSet);
      gif_buf_idx = gif_buf_last_idx;
    } else {
      gif_buf_idx += len;
    }
    file_pos += len;
    return true;
  }

  int16_t gif_buf_read(File *fd, uint8_t *dest, int16_t len) {
    while (len--) {
      if (gif_buf_idx == gif_buf_last_idx) {
        gif_buf_last_idx = fd->read(gif_buf, GIF_BUF_SIZE);
        gif_buf_idx = 0;
      }
      file_pos++;
      *(dest++) = gif_buf[gif_buf_idx++];
    }
    return len;
  }

  uint8_t gif_buf_read(File *fd) {
    if (gif_buf_idx == gif_buf_last_idx) {
      gif_buf_last_idx = fd->read(gif_buf, GIF_BUF_SIZE);
      gif_buf_idx = 0;
    }
    file_pos++;
    return gif_buf[gif_buf_idx++];
  }

  uint16_t gif_buf_read16(File *fd) {
    return gif_buf_read(fd) + (((uint16_t)gif_buf_read(fd)) << 8);
  }

  void read_palette(File *fd, gd_Palette *dest, int16_t num_colors) {
    uint8_t r, g, b;
    dest->len = num_colors;
    for (int16_t i = 0; i < num_colors; i++) {
      r = gif_buf_read(fd);
      g = gif_buf_read(fd);
      b = gif_buf_read(fd);
      dest->colors[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
    }
  }

  void discard_sub_blocks(gd_GIF *gif) {
    uint8_t len;
    do {
      gif_buf_read(gif->fd, &len, 1);
      gif_buf_seek(gif->fd, len);
    } while (len);
  }

  void read_plain_text_ext(gd_GIF *gif) {
    if (gif->plain_text) {
      uint16_t tx, ty, tw, th;
      uint8_t cw, ch, fg, bg;
      gif_buf_seek(gif->fd, 1);
      tx = gif_buf_read16(gif->fd);
      ty = gif_buf_read16(gif->fd);
      tw = gif_buf_read16(gif->fd);
      th = gif_buf_read16(gif->fd);
      cw = gif_buf_read(gif->fd);
      ch = gif_buf_read(gif->fd);
      fg = gif_buf_read(gif->fd);
      bg = gif_buf_read(gif->fd);
      gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
    } else {
      gif_buf_seek(gif->fd, 13);
    }
    discard_sub_blocks(gif);
  }

  void read_graphic_control_ext(gd_GIF *gif) {
    uint8_t rdit;
    gif_buf_seek(gif->fd, 1);
    gif_buf_read(gif->fd, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 7;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = gif_buf_read16(gif->fd);
    gif_buf_read(gif->fd, &gif->gce.tindex, 1);
    gif_buf_seek(gif->fd, 1);
  }

  void read_comment_ext(gd_GIF *gif) {
    if (gif->comment) gif->comment(gif);
    discard_sub_blocks(gif);
  }

  void read_application_ext(gd_GIF *gif) {
    char app_id[8];
    char app_auth_code[3];

    gif_buf_seek(gif->fd, 1);
    gif_buf_read(gif->fd, (uint8_t *)app_id, 8);
    gif_buf_read(gif->fd, (uint8_t *)app_auth_code, 3);

    if (!strncmp(app_id, "NETSCAPE", sizeof(app_id))) {
      gif_buf_seek(gif->fd, 2);
      gif->loop_count = gif_buf_read16(gif->fd);
      gif_buf_seek(gif->fd, 1);
    } else if (gif->application) {
      gif->application(gif, app_id, app_auth_code);
      discard_sub_blocks(gif);
    } else {
      discard_sub_blocks(gif);
    }
  }

  void read_ext(gd_GIF *gif) {
    uint8_t label;
    gif_buf_read(gif->fd, &label, 1);
    switch (label) {
      case 0x01: read_plain_text_ext(gif); break;
      case 0xF9: read_graphic_control_ext(gif); break;
      case 0xFE: read_comment_ext(gif); break;
      case 0xFF: read_application_ext(gif); break;
      default:
        discard_sub_blocks(gif);
        break;
    }
  }

  gd_Table *new_table() {
    int32_t s = (int32_t)sizeof(gd_Table) + (int32_t)(sizeof(gd_Entry) * 4096);
    gd_Table *table = (gd_Table *)malloc((size_t)s);
    if (!table) return nullptr;
    table->entries = (gd_Entry *)&table[1];
    return table;
  }

  void reset_table(gd_Table *table, uint16_t key_size) {
    table->nentries = (1 << key_size) + 2;
    for (uint16_t key = 0; key < (1 << key_size); key++) {
      table->entries[key] = (gd_Entry){1, 0xFFF, (uint8_t)key};
    }
  }

  int32_t add_entry(gd_Table *table, int32_t len, uint16_t prefix, uint8_t suffix) {
    table->entries[table->nentries] = (gd_Entry){len, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0) return 1;
    return 0;
  }

  uint16_t get_key(gd_GIF *gif, uint16_t key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte) {
    int16_t bits_read;
    int16_t rpad;
    int16_t frag_size;
    uint16_t key = 0;

    for (bits_read = 0; bits_read < (int16_t)key_size; bits_read += frag_size) {
      rpad = (*shift + bits_read) % 8;
      if (rpad == 0) {
        if (*sub_len == 0) gif_buf_read(gif->fd, sub_len, 1);
        gif_buf_read(gif->fd, byte, 1);
        (*sub_len)--;
      }
      frag_size = MIN((int16_t)key_size - bits_read, (int16_t)8 - rpad);
      key |= ((uint16_t)((*byte) >> rpad)) << bits_read;
    }
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
  }

  int16_t interlaced_line_index(int16_t h, int16_t y) {
    int16_t p;
    p = (h - 1) / 8 + 1;
    if (y < p) return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) return y * 4 + 2;
    y -= p;
    return y * 2 + 1;
  }

  int8_t read_image_data(gd_GIF *gif, int16_t interlace, uint8_t *frame) {
    uint8_t sub_len, shift, byte, table_is_full = 0;
    uint16_t init_key_size, key_size;
    int32_t frm_off, str_len = 0, p, x, y;
    uint16_t key, clear, stop;
    int32_t ret;
    gd_Entry entry = {0, 0, 0};

    gif_buf_read(gif->fd, &byte, 1);
    key_size = (uint16_t)byte;
    clear = 1 << key_size;
    stop = clear + 1;

    if (!gif->table) return -1;
    reset_table(gif->table, key_size);

    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte);
    frm_off = 0;
    ret = 0;

    while (1) {
      if (key == clear) {
        key_size = init_key_size;
        gif->table->nentries = (1 << (key_size - 1)) + 2;
        table_is_full = 0;
      } else if (!table_is_full) {
        ret = add_entry(gif->table, str_len + 1, key, entry.suffix);
        if (gif->table->nentries == 0x1000) {
          ret = 0;
          table_is_full = 1;
        }
      }

      key = get_key(gif, key_size, &sub_len, &shift, &byte);
      if (key == clear) continue;
      if (key == stop) break;
      if (ret == 1) key_size++;

      entry = gif->table->entries[key];
      str_len = entry.len;

      while (1) {
        p = frm_off + entry.len - 1;
        x = p % gif->fw;
        y = p / gif->fw;
        if (interlace) y = interlaced_line_index((int16_t)gif->fh, (int16_t)y);

        frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;

        if (entry.prefix == 0xFFF) break;
        entry = gif->table->entries[entry.prefix];
      }

      frm_off += str_len;
      if (key < (uint16_t)gif->table->nentries - 1 && !table_is_full) {
        gif->table->entries[gif->table->nentries - 1].suffix = entry.suffix;
      }
    }

    gif_buf_read(gif->fd, &sub_len, 1);
    gif->read_first_frame = true;
    return 0;
  }

  int8_t read_image(gd_GIF *gif, uint8_t *frame) {
    uint8_t fisrz;
    int16_t interlace;

    gif->fx = gif_buf_read16(gif->fd);
    gif->fy = gif_buf_read16(gif->fd);
    gif->fw = gif_buf_read16(gif->fd);
    gif->fh = gif_buf_read16(gif->fd);
    gif_buf_read(gif->fd, &fisrz, 1);

    interlace = fisrz & 0x40;
    if (fisrz & 0x80) {
      read_palette(gif->fd, &gif->lct, 1 << ((fisrz & 0x07) + 1));
      gif->palette = &gif->lct;
    } else {
      gif->palette = &gif->gct;
    }

    return read_image_data(gif, interlace, frame);
  }

  int16_t gif_buf_last_idx, gif_buf_idx;
  int32_t file_pos;
  uint8_t gif_buf[GIF_BUF_SIZE];
};

struct ThemeGifState {
  File file;
  gd_GIF *gif = nullptr;
  uint8_t *idx_frame = nullptr;
  uint16_t *canvas565 = nullptr;
  uint16_t *restore_buf = nullptr;
  size_t pixels = 0;
  size_t restore_buf_pixels = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t bg_color = 0;
  uint8_t pending_disposal = 0;
  uint16_t pending_x = 0;
  uint16_t pending_y = 0;
  uint16_t pending_w = 0;
  uint16_t pending_h = 0;
  bool pending_restore_valid = false;
  uint32_t next_frame_ms = 0;
  bool ready = false;
  bool has_frame = false;
  bool warned = false;
};

static GifClass gifClass;
static bool g_gif_fs_ready = false;
static ThemeGifState g_theme_gifs[THEME_COUNT];
static uint16_t *g_gif_draw_buf = nullptr;
static uint16_t *g_gif_xmap = nullptr;
static uint16_t *g_gif_ymap = nullptr;
static size_t g_gif_draw_pixels = 0;
static size_t g_gif_xmap_len = 0;
static size_t g_gif_ymap_len = 0;
static uint32_t g_gif_dirty_theme_mask = 0;
static uint8_t g_gif_round_robin_theme = 0;

static FS &gifFS() {
#if USE_LITTLEFS
  return LittleFS;
#else
  return SPIFFS;
#endif
}

static const char *fsName() {
#if USE_LITTLEFS
  return "LittleFS";
#else
  return "SPIFFS";
#endif
}

static void fill_canvas(uint16_t *canvas, size_t pixels, uint16_t color) {
  if (!canvas) return;
  for (size_t i = 0; i < pixels; i++) canvas[i] = color;
}

static void clearCanvasRect(uint16_t *canvas, uint16_t canvasW, uint16_t canvasH,
                            uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t color) {
  if (!canvas) return;
  if (x >= canvasW || y >= canvasH) return;
  if (x + w > canvasW) w = canvasW - x;
  if (y + h > canvasH) h = canvasH - y;
  for (uint16_t row = 0; row < h; row++) {
    uint16_t *dst = canvas + (size_t)(y + row) * canvasW + x;
    for (uint16_t col = 0; col < w; col++) dst[col] = color;
  }
}

static void copyCanvasRect(const uint16_t *canvas, uint16_t canvasW, uint16_t canvasH,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t *out) {
  if (!canvas || !out) return;
  if (x >= canvasW || y >= canvasH) return;
  if (x + w > canvasW) w = canvasW - x;
  if (y + h > canvasH) h = canvasH - y;
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t *src = canvas + (size_t)(y + row) * canvasW + x;
    uint16_t *dst = out + (size_t)row * w;
    memcpy(dst, src, (size_t)w * sizeof(uint16_t));
  }
}

static void restoreCanvasRect(uint16_t *canvas, uint16_t canvasW, uint16_t canvasH,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              const uint16_t *srcRect) {
  if (!canvas || !srcRect) return;
  if (x >= canvasW || y >= canvasH) return;
  if (x + w > canvasW) w = canvasW - x;
  if (y + h > canvasH) h = canvasH - y;
  for (uint16_t row = 0; row < h; row++) {
    uint16_t *dst = canvas + (size_t)(y + row) * canvasW + x;
    const uint16_t *src = srcRect + (size_t)row * w;
    memcpy(dst, src, (size_t)w * sizeof(uint16_t));
  }
}

static void compositeFrameRectToCanvas(gd_GIF *gif, const uint8_t *idxFrame, uint16_t *canvas565) {
  const bool hasTrans = gif->gce.transparency != 0;
  const uint8_t tindex = gif->gce.tindex;
  const uint16_t *colors = gif->palette->colors;

  for (uint16_t j = 0; j < gif->fh; j++) {
    size_t rowBase = (size_t)(gif->fy + j) * gif->width + gif->fx;
    for (uint16_t k = 0; k < gif->fw; k++) {
      uint8_t idx = idxFrame[rowBase + k];
      if (!hasTrans || idx != tindex) {
        canvas565[rowBase + k] = colors[idx];
      }
    }
  }
}

static void scaleCanvasToOutbuf(const uint16_t *canvas, uint16_t inW, uint16_t inH,
                                uint16_t *outbuf, int outW, int outH,
                                const uint16_t *xmap, const uint16_t *ymap) {
  for (int y = 0; y < outH; y++) {
    uint16_t sy = ymap[y];
    const uint16_t *srcRow = canvas + (size_t)sy * inW;
    uint16_t *dstRow = outbuf + (size_t)y * outW;
    for (int x = 0; x < outW; x++) {
      dstRow[x] = srcRow[xmap[x]];
    }
  }
}

static void release_theme_gif(ThemeGifState &state) {
  if (state.gif) gifClass.gd_close_gif(state.gif);
  if (state.idx_frame) free(state.idx_frame);
  if (state.canvas565) free(state.canvas565);
  if (state.restore_buf) free(state.restore_buf);
  state.file = File();
  state.gif = nullptr;
  state.idx_frame = nullptr;
  state.canvas565 = nullptr;
  state.restore_buf = nullptr;
  state.pixels = 0;
  state.restore_buf_pixels = 0;
  state.width = 0;
  state.height = 0;
  state.bg_color = 0;
  state.pending_disposal = 0;
  state.pending_x = 0;
  state.pending_y = 0;
  state.pending_w = 0;
  state.pending_h = 0;
  state.pending_restore_valid = false;
  state.next_frame_ms = 0;
  state.ready = false;
  state.has_frame = false;
  state.warned = false;
}

static bool ensure_gif_draw_buffers(int outW, int outH) {
  size_t need_pixels = (size_t)outW * (size_t)outH;
  if (need_pixels > g_gif_draw_pixels) {
    if (g_gif_draw_buf) free(g_gif_draw_buf);
    g_gif_draw_buf = (uint16_t *)gif_alloc(need_pixels * sizeof(uint16_t));
    if (!g_gif_draw_buf) {
      g_gif_draw_pixels = 0;
      return false;
    }
    g_gif_draw_pixels = need_pixels;
  }
  if ((size_t)outW > g_gif_xmap_len) {
    if (g_gif_xmap) free(g_gif_xmap);
    g_gif_xmap = (uint16_t *)gif_alloc((size_t)outW * sizeof(uint16_t));
    if (!g_gif_xmap) {
      g_gif_xmap_len = 0;
      return false;
    }
    g_gif_xmap_len = (size_t)outW;
  }
  if ((size_t)outH > g_gif_ymap_len) {
    if (g_gif_ymap) free(g_gif_ymap);
    g_gif_ymap = (uint16_t *)gif_alloc((size_t)outH * sizeof(uint16_t));
    if (!g_gif_ymap) {
      g_gif_ymap_len = 0;
      return false;
    }
    g_gif_ymap_len = (size_t)outH;
  }
  return g_gif_draw_buf && g_gif_xmap && g_gif_ymap;
}

static bool init_theme_gif(uint8_t theme_id, uint16_t bg_color) {
  if (!g_gif_fs_ready || theme_id >= THEME_COUNT) return false;
  const char *path = GIF_ASSET_PATHS[theme_id];
  if (!path) return false;

  ThemeGifState &state = g_theme_gifs[theme_id];
  if (state.ready && state.bg_color == bg_color) return true;

  release_theme_gif(state);

  state.file = gifFS().open(path, "r");
  if (!state.file || state.file.isDirectory()) {
    if (!state.warned) {
      Serial.printf("[GIF] Missing asset: %s\n", path);
      state.warned = true;
    }
    state.file = File();
    return false;
  }

  state.gif = gifClass.gd_open_gif(&state.file);
  if (!state.gif) {
    Serial.printf("[GIF] Failed to open GIF: %s\n", path);
    state.file.close();
    state.file = File();
    return false;
  }

  state.width = state.gif->width;
  state.height = state.gif->height;
  state.pixels = (size_t)state.width * (size_t)state.height;
  state.idx_frame = (uint8_t *)gif_alloc(state.pixels);
  state.canvas565 = (uint16_t *)gif_alloc(state.pixels * sizeof(uint16_t));
  if (!state.idx_frame || !state.canvas565) {
    Serial.printf("[GIF] OOM preparing %s (%ux%u)\n", path, (unsigned)state.width, (unsigned)state.height);
    release_theme_gif(state);
    return false;
  }

  state.bg_color = bg_color;
  fill_canvas(state.canvas565, state.pixels, bg_color);
  state.ready = true;
  state.has_frame = false;
  state.next_frame_ms = 0;
  return true;
}

static void apply_pending_disposal(ThemeGifState &state) {
  if (!state.ready || !state.canvas565 || state.pending_disposal == 0) return;

  if (state.pending_disposal == 2) {
    clearCanvasRect(state.canvas565, state.width, state.height,
                    state.pending_x, state.pending_y, state.pending_w, state.pending_h,
                    state.bg_color);
  } else if (state.pending_disposal == 3) {
    if (state.pending_restore_valid && state.restore_buf) {
      restoreCanvasRect(state.canvas565, state.width, state.height,
                        state.pending_x, state.pending_y, state.pending_w, state.pending_h,
                        state.restore_buf);
    } else {
      clearCanvasRect(state.canvas565, state.width, state.height,
                      state.pending_x, state.pending_y, state.pending_w, state.pending_h,
                      state.bg_color);
    }
  }

  state.pending_disposal = 0;
  state.pending_restore_valid = false;
}

static bool advance_theme_gif(uint8_t theme_id, uint32_t now) {
  if (theme_id >= THEME_COUNT) return false;
  ThemeGifState &state = g_theme_gifs[theme_id];
  if (!state.ready || !state.gif || !state.canvas565 || !state.idx_frame) return false;
  if (state.has_frame && (int32_t)(now - state.next_frame_ms) < 0) return false;

  apply_pending_disposal(state);

  int32_t res = gifClass.gd_get_frame(state.gif, state.idx_frame);
  if (res == 0) {
    gifClass.gd_rewind(state.gif);
    fill_canvas(state.canvas565, state.pixels, state.bg_color);
    res = gifClass.gd_get_frame(state.gif, state.idx_frame);
  }
  if (res < 0) {
    Serial.printf("[GIF] Decode error on theme %u\n", (unsigned)theme_id);
    release_theme_gif(state);
    return false;
  }

  bool restore_valid = false;
  if (state.gif->gce.disposal == 3) {
    size_t need = (size_t)state.gif->fw * (size_t)state.gif->fh;
    if (need > state.restore_buf_pixels) {
      if (state.restore_buf) free(state.restore_buf);
      state.restore_buf = (uint16_t *)gif_alloc(need * sizeof(uint16_t));
      state.restore_buf_pixels = state.restore_buf ? need : 0;
    }
    if (state.restore_buf) {
      copyCanvasRect(state.canvas565, state.width, state.height,
                     state.gif->fx, state.gif->fy, state.gif->fw, state.gif->fh,
                     state.restore_buf);
      restore_valid = true;
    }
  }

  compositeFrameRectToCanvas(state.gif, state.idx_frame, state.canvas565);
  state.pending_disposal = state.gif->gce.disposal;
  state.pending_x = state.gif->fx;
  state.pending_y = state.gif->fy;
  state.pending_w = state.gif->fw;
  state.pending_h = state.gif->fh;
  state.pending_restore_valid = restore_valid;
  state.has_frame = true;

  uint32_t delay_ms = (uint32_t)state.gif->gce.delay * 10;
  if (delay_ms < MIN_GIF_FRAME_DELAY_MS) delay_ms = MIN_GIF_FRAME_DELAY_MS;
  state.next_frame_ms = now + delay_ms;
  return true;
}

static bool prepare_theme_gif(uint8_t theme_id, uint16_t bg_color, uint32_t now) {
  if (!init_theme_gif(theme_id, bg_color)) return false;
  ThemeGifState &state = g_theme_gifs[theme_id];
  if (!state.has_frame || (int32_t)(now - state.next_frame_ms) >= 0) {
    return advance_theme_gif(theme_id, now);
  }
  return false;
}

static bool draw_theme_gif(uint8_t theme_id, int box_x, int box_y, int box_w, int box_h) {
  if (theme_id >= THEME_COUNT || box_w <= 0 || box_h <= 0) return false;
  ThemeGifState &state = g_theme_gifs[theme_id];
  if (!state.ready || !state.has_frame || !state.canvas565) return false;

  int outW = box_w;
  int outH = (int)((int64_t)box_w * (int64_t)state.height / (int64_t)state.width);
  if (outH > box_h) {
    outH = box_h;
    outW = (int)((int64_t)box_h * (int64_t)state.width / (int64_t)state.height);
  }
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;

  if (!ensure_gif_draw_buffers(outW, outH)) return false;

  for (int x = 0; x < outW; x++) {
    g_gif_xmap[x] = (uint16_t)(((uint32_t)x * (uint32_t)state.width) / (uint32_t)outW);
  }
  for (int y = 0; y < outH; y++) {
    g_gif_ymap[y] = (uint16_t)(((uint32_t)y * (uint32_t)state.height) / (uint32_t)outH);
  }

  scaleCanvasToOutbuf(state.canvas565, state.width, state.height,
                      g_gif_draw_buf, outW, outH, g_gif_xmap, g_gif_ymap);

  int draw_x = box_x + (box_w - outW) / 2;
  int draw_y = box_y + (box_h - outH) / 2;

  gfx->startWrite();
  gfx->draw16bitRGBBitmap(draw_x - BITMAP_X_ALIGN_FIX, draw_y, g_gif_draw_buf, outW, outH);
  gfx->endWrite();
  return true;
}

static bool sync_theme_gifs(uint32_t now) {
  bool any_changed = false;
  bool needed[THEME_COUNT] = {false};
  bool theme_due[THEME_COUNT] = {false};
  uint8_t chosen_theme_id = THEME_COUNT;

  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!timers[i].active) continue;
    if (timers[i].theme_id < THEME_COUNT && GIF_ASSET_PATHS[timers[i].theme_id]) {
      chosen_theme_id = timers[i].theme_id;
      break;
    }
  }

  if (chosen_theme_id < THEME_COUNT) {
    needed[chosen_theme_id] = true;
    TimerTheme theme = theme_from_id(chosen_theme_id);
    if (init_theme_gif(chosen_theme_id, theme.bg)) {
      ThemeGifState &state = g_theme_gifs[chosen_theme_id];
      if (!state.has_frame || (int32_t)(now - state.next_frame_ms) >= 0) {
        theme_due[chosen_theme_id] = true;
      }
    }
  }

  for (uint8_t offset = 0; offset < THEME_COUNT; offset++) {
    uint8_t theme_id = (uint8_t)((g_gif_round_robin_theme + offset) % THEME_COUNT);
    if (!theme_due[theme_id]) continue;
    if (advance_theme_gif(theme_id, now)) {
      any_changed = true;
      if (theme_id < 32) g_gif_dirty_theme_mask |= (1UL << theme_id);
    }
    g_gif_round_robin_theme = (uint8_t)((theme_id + 1) % THEME_COUNT);
    break;
  }

  for (uint8_t theme_id = 0; theme_id < THEME_COUNT; theme_id++) {
    if (!needed[theme_id]) release_theme_gif(g_theme_gifs[theme_id]);
  }
  return any_changed;
}
#endif

// ======================= SOUND EFFECTS =======================
#if USE_SPEAKER
enum SfxId {
  SFX_POWER_ON = 0,
  SFX_CONFIRM = 1,
  SFX_PAUSE = 2,
  SFX_RESUME = 3,
  SFX_PAUSE_RESUME = 4
};

static bool sfx_playing = false;

struct BeepStep {
  uint16_t freq_hz;   // 0 = silence
  uint16_t dur_ms;    // 0 = end
};

static const BeepStep SFX_PWRON[] = {
  { 880,  70 }, { 0, 25 }, { 1175, 70 }, { 0, 25 }, { 1568, 90 }, { 0, 0 }
};
static const BeepStep SFX_OK[] = {
  { 1175, 60 }, { 0, 25 }, { 1568, 70 }, { 0, 0 }
};
static const BeepStep SFX_CANCEL[] = {
  { 784,  80 }, { 0, 25 }, { 587, 120 }, { 0, 0 }
};
static const BeepStep SFX_RES[] = {
  { 988,  60 }, { 0, 25 }, { 1319, 70 }, { 0, 0 }
};
static const BeepStep SFX_TOGGLE[] = {
  { 1319, 55 }, { 0, 20 }, { 988, 55 }, { 0, 0 }
};

static const BeepStep* g_beep_steps = nullptr;
static uint8_t g_beep_idx = 0;
static uint32_t g_beep_next_ms = 0;

static void sfx_stop() {
  #if USE_MP3_SFX
  if (g_mp3) {
    if (g_mp3->isRunning()) g_mp3->stop();
    delete g_mp3;
    g_mp3 = nullptr;
  }
  if (g_src) {
    delete g_src;
    g_src = nullptr;
  }
  #endif

  // Stop PWM tone output
  ledcWriteTone(AUDIO_PIN, 0);
  ledcWrite(AUDIO_PIN, 0);
  g_beep_steps = nullptr;
  g_beep_idx = 0;
  sfx_playing = false;
}

static void sfx_play(int idx) {
  sfx_stop();

  #if USE_MP3_SFX
  if (SFX_MP3_COUNT == 0) return;
  if (idx < 0 || idx >= (int)SFX_MP3_COUNT) return;

  if (g_out) {
    if (g_sfx_gain < 0.0f) g_sfx_gain = 0.0f;
    if (g_sfx_gain > 1.0f) g_sfx_gain = 1.0f;
    g_out->SetGain(g_sfx_gain);
  }

  g_mp3 = new AudioGeneratorMP3();
  g_src = new AudioFileSourcePROGMEM(SFX_MP3_LIST[idx].data, SFX_MP3_LIST[idx].len);
  if (g_mp3->begin(g_src, g_out)) {
    sfx_playing = true;
    Serial.printf("[SFX] Playing: %s\n", SFX_MP3_LIST[idx].name);
  }
  #else
  // PWM-based beep SFX (reliable on analog/PWM speaker input)
  const BeepStep* steps = nullptr;
  switch (idx) {
    case SFX_POWER_ON: steps = SFX_PWRON; break;
    case SFX_CONFIRM: steps = SFX_OK; break;
    case SFX_PAUSE: steps = SFX_CANCEL; break;
    case SFX_RESUME: steps = SFX_RES; break;
    case SFX_PAUSE_RESUME: steps = SFX_TOGGLE; break;
    default: steps = SFX_OK; break;
  }
  g_beep_steps = steps;
  g_beep_idx = 0;
  g_beep_next_ms = millis();
  sfx_playing = true;
  #endif
}

static void sfx_loop() {
  #if USE_MP3_SFX
  if (g_mp3 && g_mp3->isRunning()) {
    if (!g_mp3->loop()) {
      g_mp3->stop();
      sfx_playing = false;
    }
  }
  #else
  if (!sfx_playing || !g_beep_steps) return;

  const uint32_t now = millis();
  if ((int32_t)(now - g_beep_next_ms) < 0) return;

  const BeepStep step = g_beep_steps[g_beep_idx++];
  if (step.dur_ms == 0) {
    ledcWriteTone(AUDIO_PIN, 0);
    ledcWrite(AUDIO_PIN, 0);
    sfx_playing = false;
    g_beep_steps = nullptr;
    return;
  }

  if (step.freq_hz == 0) {
    ledcWriteTone(AUDIO_PIN, 0);
    ledcWrite(AUDIO_PIN, 0);
  } else {
    ledcWriteTone(AUDIO_PIN, step.freq_hz);
    ledcWrite(AUDIO_PIN, g_pwm_duty);
  }
  g_beep_next_ms = now + step.dur_ms;
  #endif
}

// Simple alarm tone using PWM (backup if MP3 fails)
static void play_alarm_tone(bool enable) {
  // Use ledcWriteTone for simple beeping when no MP3 available
  static bool alarm_state = false;
  static uint32_t last_toggle = 0;
  
  if (!enable) {
    ledcWriteTone(AUDIO_PIN, 0);
    ledcWrite(AUDIO_PIN, 0);
    alarm_state = false;
    return;
  }
  
  uint32_t now = millis();
  if (now - last_toggle > 500) {
    last_toggle = now;
    alarm_state = !alarm_state;
    ledcWriteTone(AUDIO_PIN, alarm_state ? 880 : 0);  // 880Hz beep
    ledcWrite(AUDIO_PIN, alarm_state ? g_pwm_duty : 0);
  }
}
#endif

// ======================= COMMAND PARSING (for BLE & Demo Mode) =======================
// Parse command from BLE/Demo message
// Format: "CMD:SET,NAME:Timer 1,DURATION:300" (seconds)
//         "CMD:CANCEL,NAME:Timer 1"
//         "CMD:ADD,NAME:Timer 1,DURATION:60"
//         "CMD:MINUS,NAME:Timer 1,DURATION:60"
//         "CMD:STOP"

static ParsedCommand parseCommand(const std::string& msg) {
  ParsedCommand result = {CMD_NONE, "", 0};
  
  if (msg.find("CMD:SET") != std::string::npos) {
    result.cmd = CMD_SET;
  } else if (msg.find("CMD:CANCEL") != std::string::npos) {
    result.cmd = CMD_CANCEL;
  } else if (msg.find("CMD:ADD") != std::string::npos) {
    result.cmd = CMD_ADD;
  } else if (msg.find("CMD:MINUS") != std::string::npos) {
    result.cmd = CMD_MINUS;
  } else if (msg.find("CMD:STOP") != std::string::npos) {
    result.cmd = CMD_STOP;
    return result;
  }
  
  // Parse NAME
  size_t namePos = msg.find("NAME:");
  if (namePos != std::string::npos) {
    size_t start = namePos + 5;
    size_t end = msg.find(",", start);
    if (end == std::string::npos) end = msg.length();
    std::string name = msg.substr(start, end - start);
    strncpy(result.name, name.c_str(), 15);
    result.name[15] = '\0';
  }
  
  // Parse DURATION
  size_t durPos = msg.find("DURATION:");
  if (durPos != std::string::npos) {
    size_t start = durPos + 9;
    result.duration = atoi(msg.substr(start).c_str());
  }
  
  return result;
}

void processVoiceCommand(ParsedCommand& cmd);  // Forward declaration
static void requestFullRedraw();               // Forward declaration

// ======================= BLE FOR NICLA VOICE =======================
#if USE_BLE
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* rxChar = nullptr;
NimBLECharacteristic* txChar = nullptr;
bool deviceConnected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("[BLE] Device connected");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("[BLE] Device disconnected");
    NimBLEDevice::startAdvertising();
  }
};

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      Serial.printf("[BLE] Received: %s\n", value.c_str());
      
      ParsedCommand cmd = parseCommand(value);
      processVoiceCommand(cmd);
    }
  }
};
#endif

// ======================= TIMER MANAGEMENT =======================
static int findTimerByName(const char* name) {
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (timers[i].active && strcmp(timers[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static int findFreeTimerSlot() {
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (!timers[i].active) {
      return i;
    }
  }
  return -1;
}

static void updateActiveCount() {
  active_timer_count = 0;
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (timers[i].active) active_timer_count++;
  }
}

static bool createTimer(const char* name, uint32_t duration_seconds) {
  if (active_timer_count >= MAX_TIMERS) {
    Serial.println("[TIMER] Max timers reached");
    return false;
  }
  
  int slot = findFreeTimerSlot();
  if (slot < 0) return false;
  
  strncpy(timers[slot].name, name, 15);
  timers[slot].name[15] = '\0';
  timers[slot].total_seconds = duration_seconds;
  timers[slot].seconds_left = duration_seconds;
  timers[slot].active = true;
  timers[slot].ringing = false;
  timers[slot].theme_id = detect_theme_id(name);
  
  updateActiveCount();
  requestFullRedraw();
  Serial.printf("[TIMER] Created: %s for %lu seconds\n", name, (unsigned long)duration_seconds);
  
  #if USE_SPEAKER
  sfx_play(SFX_CONFIRM);
  #endif
  
  return true;
}

static bool cancelTimer(const char* name) {
  int idx = findTimerByName(name);
  if (idx < 0) {
    Serial.printf("[TIMER] Not found: %s\n", name);
    return false;
  }
  
  timers[idx].active = false;
  timers[idx].ringing = false;
  updateActiveCount();
  requestFullRedraw();
  
  Serial.printf("[TIMER] Cancelled: %s\n", name);
  
  #if USE_SPEAKER
  sfx_play(SFX_PAUSE);
  #endif
  
  return true;
}

static bool addTimeToTimer(const char* name, uint32_t seconds) {
  int idx = findTimerByName(name);
  if (idx < 0) return false;
  
  timers[idx].seconds_left += seconds;
  timers[idx].total_seconds += seconds;
  
  if (timers[idx].ringing) {
    timers[idx].ringing = false;  // Stop alarm if adding time
  }
  requestFullRedraw();
  
  Serial.printf("[TIMER] Added %lu seconds to %s\n", (unsigned long)seconds, name);
  
  #if USE_SPEAKER
  sfx_play(SFX_CONFIRM);
  #endif
  
  return true;
}

static bool subtractTimeFromTimer(const char* name, uint32_t seconds) {
  int idx = findTimerByName(name);
  if (idx < 0) return false;
  
  if (seconds >= timers[idx].seconds_left) {
    timers[idx].seconds_left = 0;
  } else {
    timers[idx].seconds_left -= seconds;
  }
  requestFullRedraw();
  
  Serial.printf("[TIMER] Subtracted %lu seconds from %s\n", (unsigned long)seconds, name);
  
  #if USE_SPEAKER
  sfx_play(SFX_CONFIRM);
  #endif
  
  return true;
}

static void stopAllAlarms() {
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (timers[i].ringing) {
      timers[i].ringing = false;
      timers[i].active = false;  // Remove timer after stopping alarm
    }
  }
  updateActiveCount();
  requestFullRedraw();
  
  Serial.println("[TIMER] All alarms stopped");
  
  #if USE_SPEAKER
  sfx_stop();
  play_alarm_tone(false);
  #endif
}

// ======================= VOICE COMMAND PROCESSING =======================
// Moved outside USE_BLE to support both BLE and Demo modes
void processVoiceCommand(ParsedCommand& cmd) {
  switch (cmd.cmd) {
    case CMD_SET:
      createTimer(cmd.name, cmd.duration);
      break;
    case CMD_CANCEL:
      cancelTimer(cmd.name);
      break;
    case CMD_ADD:
      addTimeToTimer(cmd.name, cmd.duration);
      break;
    case CMD_MINUS:
      subtractTimeFromTimer(cmd.name, cmd.duration);
      break;
    case CMD_STOP:
      stopAllAlarms();
      break;
    default:
      break;
  }
}

// ======================= DISPLAY HELPERS =======================
static void fmt_hhmmss(uint32_t sec, char* out) {
  uint32_t m = sec / 60, s = sec % 60;
  uint32_t h = m / 60;
  m = m % 60;
  sprintf(out, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

struct PanelLayout {
  int x;
  int y;
  int w;
  int h;
  int title_anchor_x;
  int title_y;
  uint8_t title_scale;
  bool title_centered;
  int time_anchor_x;
  int time_y;
  uint8_t time_scale;
  bool time_centered;
  bool show_art;
  int art_box_x;
  int art_box_y;
  int art_box_w;
  int art_box_h;
  int ring_cx;
  int ring_cy;
  float ring_scale;
};

static void configure_ui_font(uint8_t scale) {
  gfx->setFont(&TIMER_UI_FONT_FAMILY);
  gfx->setTextSize(scale);
  gfx->setTextWrap(false);
}

static void measure_ui_text(const char* text, uint8_t scale,
                            int16_t* x1, int16_t* y1,
                            uint16_t* w, uint16_t* h) {
  configure_ui_font(scale);
  gfx->getTextBounds(text, 0, 0, x1, y1, w, h);
  gfx->setFont(nullptr);
  gfx->setTextSize(1);
}

static void draw_ui_text(const char* text, int left, int top, uint16_t color, uint8_t scale) {
  int16_t x1, y1;
  uint16_t w, h;
  measure_ui_text(text, scale, &x1, &y1, &w, &h);
  configure_ui_font(scale);
  gfx->setTextColor(color);
  gfx->setCursor(left - x1, top - y1);
  gfx->print(text);
  gfx->setFont(nullptr);
  gfx->setTextSize(1);
}

static void draw_ui_text_centered(const char* text, int center_x, int top, uint16_t color, uint8_t scale) {
  int16_t x1, y1;
  uint16_t w, h;
  measure_ui_text(text, scale, &x1, &y1, &w, &h);
  int left = center_x - ((int)w / 2);
  draw_ui_text(text, left, top, color, scale);
}

static void draw_ui_text_layout(const char* text, int anchor_x, int top,
                                bool centered, uint16_t color, uint8_t scale) {
  if (centered) {
    draw_ui_text_centered(text, anchor_x, top, color, scale);
  } else {
    draw_ui_text(text, anchor_x, top, color, scale);
  }
}

static PanelLayout panel_layout_for(int active_count, int slot) {
  PanelLayout layout = {};
  layout.y = 0;
  layout.h = 320;

  if (active_count == 1) {
    layout.x = 0;
    layout.w = 960;
    layout.title_anchor_x = 54;
    layout.title_y = 58;
    layout.title_scale = 1;
    layout.title_centered = false;
    layout.time_anchor_x = 54;
    layout.time_y = 126;
    layout.time_scale = 2;
    layout.time_centered = false;
    layout.show_art = true;
    layout.art_box_x = 710;
    layout.art_box_y = 110;
    layout.art_box_w = 210;
    layout.art_box_h = 180;
    layout.ring_cx = 536;
    layout.ring_cy = 165;
    layout.ring_scale = 0.98f;
  } else if (active_count == 2) {
    layout.x = slot * 480;
    layout.w = 480;
    layout.title_anchor_x = layout.x + 26;
    layout.title_y = 30;
    layout.title_scale = 1;
    layout.title_centered = false;
    layout.time_anchor_x = layout.x + 26;
    layout.time_y = 78;
    layout.time_scale = 1;
    layout.time_centered = false;
    layout.show_art = true;
    layout.art_box_x = layout.x + 18;
    layout.art_box_y = 154;
    layout.art_box_w = 208;
    layout.art_box_h = 136;
    layout.ring_cx = layout.x + 356;
    layout.ring_cy = 164;
    layout.ring_scale = 0.84f;
  } else {
    layout.x = slot * 320;
    layout.w = 320;
    layout.title_anchor_x = layout.x + 160;
    layout.title_y = 22;
    layout.title_scale = 1;
    layout.title_centered = true;
    layout.time_anchor_x = layout.x + 160;
    layout.time_y = 58;
    layout.time_scale = 1;
    layout.time_centered = true;
    layout.show_art = false;
    layout.art_box_x = 0;
    layout.art_box_y = 0;
    layout.art_box_w = 0;
    layout.art_box_h = 0;
    layout.ring_cx = layout.x + 160;
    layout.ring_cy = 218;
    layout.ring_scale = 0.66f;
  }

  return layout;
}

static void draw_clock_illustration(int cx, int cy, int size, const TimerTheme& theme) {
  gfx->fillCircle(cx, cy, size / 2, theme.art_shadow);
  gfx->fillCircle(cx, cy, size / 2 - 8, theme.art_primary);
  gfx->fillCircle(cx, cy, size / 2 - 18, theme.bg);
  gfx->drawLine(cx, cy, cx, cy - size / 5, theme.art_secondary);
  gfx->drawLine(cx, cy, cx + size / 6, cy + size / 10, theme.art_secondary);
  gfx->fillCircle(cx, cy, 4, theme.art_secondary);
}

static void draw_coffee_illustration(int cx, int cy, int size, const TimerTheme& theme) {
  int saucer_y = cy + size / 5;
  gfx->fillCircle(cx, saucer_y, size / 2, theme.art_shadow);
  gfx->fillCircle(cx, saucer_y - 2, size / 2 - 8, theme.art_secondary);
  gfx->fillCircle(cx, saucer_y - 2, size / 2 - 18, theme.art_primary);
  gfx->fillRoundRect(cx - size / 5, cy - size / 6, size / 2, size / 3, size / 10, theme.art_primary);
  gfx->fillCircle(cx, cy - size / 7, size / 7, theme.art_tertiary);
  gfx->drawCircle(cx + size / 7, cy - size / 24, size / 9, theme.art_secondary);
  gfx->drawCircle(cx + size / 7, cy - size / 24, size / 9 + 1, theme.art_secondary);
}

static void draw_books_illustration(int cx, int cy, int size, const TimerTheme& theme) {
  int book_w = size / 3;
  int book_h = size / 2;
  gfx->fillRoundRect(cx - book_w, cy - book_h / 2, book_w, book_h, 8, theme.art_primary);
  gfx->fillRoundRect(cx - book_w / 8, cy - book_h / 2 + size / 12, book_w, book_h, 8, theme.art_secondary);
  gfx->fillRect(cx - book_w + book_w / 5, cy - book_h / 2 + 10, 4, book_h - 20, theme.art_shadow);
  gfx->fillRect(cx + book_w / 7, cy - book_h / 2 + 16, 4, book_h - 26, theme.art_shadow);
  gfx->drawLine(cx - book_w / 2, cy - book_h / 4, cx - book_w / 6, cy - book_h / 3, theme.art_tertiary);
  gfx->drawLine(cx - book_w / 2, cy - book_h / 4 + 8, cx - book_w / 7, cy - book_h / 3 + 8, theme.art_tertiary);
}

static void draw_baking_illustration(int cx, int cy, int size, const TimerTheme& theme) {
  int bowl_w = (size * 11) / 14;
  int bowl_h = size / 2;
  int bowl_y = cy + size / 6;
  int bowl_x = cx - bowl_w / 2;
  int rim_h = bowl_h / 4;

  gfx->fillRoundRect(bowl_x, bowl_y - bowl_h / 2, bowl_w, bowl_h, bowl_h / 2, theme.art_primary);
  gfx->fillRoundRect(bowl_x + 8, bowl_y - bowl_h / 2 + 8, bowl_w - 16, bowl_h - 10, bowl_h / 2, hex565(0xff7f73));
  gfx->fillRoundRect(bowl_x + 18, bowl_y - bowl_h / 2 + 12, bowl_w - 36, rim_h + 10, rim_h / 2, COLOR_TEXT_WHITE);
  gfx->drawFastHLine(bowl_x + 20, bowl_y - bowl_h / 2 + rim_h + 8, bowl_w - 40, lerp565(theme.art_primary, theme.art_shadow, 140));

  int whisk_x = cx + bowl_w / 6;
  int whisk_top = cy - size / 3;
  int whisk_bottom = bowl_y + bowl_h / 8;
  gfx->fillRoundRect(whisk_x - 6, whisk_top, 12, (whisk_bottom - whisk_top) - 4, 6, theme.art_tertiary);
  gfx->drawLine(whisk_x - 14, whisk_bottom, whisk_x - 3, whisk_bottom + 30, theme.art_shadow);
  gfx->drawLine(whisk_x - 6, whisk_bottom - 2, whisk_x + 2, whisk_bottom + 30, theme.art_shadow);
  gfx->drawLine(whisk_x + 2, whisk_bottom - 2, whisk_x + 10, whisk_bottom + 28, theme.art_shadow);
  gfx->drawLine(whisk_x + 10, whisk_bottom - 4, whisk_x + 18, whisk_bottom + 24, theme.art_shadow);

  gfx->fillCircle(cx - bowl_w / 2 + 18, cy - 4, 3, COLOR_TEXT_WHITE);
  gfx->fillCircle(cx - bowl_w / 2 + 42, cy - 28, 2, COLOR_TEXT_WHITE);
  gfx->drawCircle(cx + bowl_w / 2 - 20, cy - 34, 5, COLOR_TEXT_WHITE);
  gfx->drawCircle(cx + bowl_w / 2 + 8, cy - 14, 3, COLOR_TEXT_WHITE);
}

static void draw_dumbbell_illustration(int cx, int cy, int size, const TimerTheme& theme) {
  gfx->fillRect(cx - size / 4, cy - size / 16, size / 2, size / 8, theme.art_secondary);
  gfx->fillCircle(cx - size / 3, cy, size / 10, theme.art_primary);
  gfx->fillCircle(cx - size / 4, cy, size / 7, theme.art_primary);
  gfx->fillCircle(cx + size / 4, cy, size / 7, theme.art_primary);
  gfx->fillCircle(cx + size / 3, cy, size / 10, theme.art_primary);
  gfx->fillRect(cx - size / 3, cy - size / 12, size / 16, size / 6, theme.art_tertiary);
  gfx->fillRect(cx + size / 4, cy - size / 12, size / 16, size / 6, theme.art_tertiary);
}

static void draw_theme_illustration(uint8_t theme_id, int cx, int cy, int size, const TimerTheme& theme) {
  switch (theme_id) {
    case THEME_BREAK:
      draw_coffee_illustration(cx, cy, size, theme);
      break;
    case THEME_HOMEWORK:
      draw_books_illustration(cx, cy, size, theme);
      break;
    case THEME_BAKING:
      draw_baking_illustration(cx, cy, size, theme);
      break;
    case THEME_EXERCISE:
      draw_dumbbell_illustration(cx, cy, size, theme);
      break;
    default:
      draw_clock_illustration(cx, cy, size, theme);
      break;
  }
}

static void draw_timer_art(const PanelLayout& layout, const Timer& timer, const TimerTheme& theme) {
  if (!layout.show_art) return;

  #if USE_GIFS
  if (draw_theme_gif(timer.theme_id,
                     layout.art_box_x,
                     layout.art_box_y,
                     layout.art_box_w,
                     layout.art_box_h)) {
    return;
  }
  #endif

  int size = layout.art_box_w < layout.art_box_h ? layout.art_box_w : layout.art_box_h;
  draw_theme_illustration(timer.theme_id,
                          layout.art_box_x + layout.art_box_w / 2,
                          layout.art_box_y + layout.art_box_h / 2,
                          size,
                          theme);
}

static void draw_panel_art_accents(const PanelLayout& layout, const TimerTheme& theme) {
  if (!layout.show_art) return;
  gfx->fillCircle(layout.art_box_x + layout.art_box_w - 26,
                  layout.art_box_y + 20,
                  7,
                  lerp565(theme.ring_start, theme.art_primary, 110));
  gfx->fillCircle(layout.art_box_x + layout.art_box_w - 48,
                  layout.art_box_y + 44,
                  4,
                  lerp565(theme.ring_start, theme.art_secondary, 100));
}

static void draw_panel_backdrop(const PanelLayout& layout, const TimerTheme& theme) {
  gfx->fillCircle(layout.ring_cx, layout.ring_cy,
                  ring_size_px(layout.ring_scale) / 2 + 10,
                  lerp565(theme.bg, theme.ring_empty, 72));
  draw_panel_art_accents(layout, theme);
}

static void draw_timer_ring(const PanelLayout& layout, const Timer& timer, float frac, uint32_t now) {
  #if USE_RING
  TimerTheme theme = resolved_theme_for_timer(timer);
  bool flash = ((now / 250) % 2) == 0;
  uint16_t ring_start = timer.ringing
    ? (flash ? COLOR_ALERT_START : lerp565(COLOR_ALERT_START, COLOR_ALERT_ORANGE, 80))
    : theme.ring_start;
  uint16_t ring_end = timer.ringing
    ? (flash ? COLOR_ALERT_RED : COLOR_ALERT_ORANGE)
    : theme.ring_end;
  uint16_t ring_empty = timer.ringing
    ? lerp565(theme.ring_empty, COLOR_ALERT_ORANGE, 80)
    : theme.ring_empty;
  float draw_frac = timer.ringing ? (flash ? 0.04f : 0.12f) : frac;
  int ring_size = ring_size_px(layout.ring_scale);

  ensure_ring_lut(layout.ring_scale);
  draw_ring(draw_frac, CAP_LEAD, ring_start, ring_end, ring_empty,
            layout.ring_cx - ring_size / 2,
            layout.ring_cy - ring_size / 2,
            theme.bg, layout.ring_scale);
  #endif
}

static void draw_timer_panel(const PanelLayout& layout, const Timer& timer, uint32_t now) {
  TimerTheme theme = resolved_theme_for_timer(timer);
  char hhmmss[9];
  fmt_hhmmss(timer.seconds_left, hhmmss);

  gfx->fillRect(layout.x, layout.y, layout.w, layout.h, theme.bg);
  draw_panel_backdrop(layout, theme);
  draw_timer_art(layout, timer, theme);
  draw_ui_text_layout(timer.name, layout.title_anchor_x, layout.title_y,
                      layout.title_centered, theme.text, layout.title_scale);
  draw_ui_text_layout(hhmmss, layout.time_anchor_x, layout.time_y,
                      layout.time_centered, theme.text, layout.time_scale);

  float frac = 0.0f;
  if (timer.total_seconds > 0) {
    frac = (float)timer.seconds_left / (float)timer.total_seconds;
  }
  draw_timer_ring(layout, timer, frac, now);
}

static void drawNoTimersScreen(uint32_t now) {
  gfx->fillScreen(COLOR_IDLE_BG);
  draw_idle_border(now);
  draw_ui_text_centered("No Active Timers", 480, 102, COLOR_TEXT_WHITE, 2);
  draw_ui_text_centered("Say 'Set a timer' to begin", 480, 176, COLOR_IDLE_BORDER, 1);
}

static void redrawNoTimersBorder(uint32_t now) {
  clear_idle_border_band();
  draw_idle_border(now);
}

static void clear_timer_time_region(const PanelLayout& layout, uint16_t bg) {
  int clear_x;
  int clear_w;
  if (layout.time_centered) {
    clear_x = layout.x + 24;
    clear_w = layout.w - 48;
  } else {
    clear_x = layout.x + 20;
    int ring_left = layout.ring_cx - ring_size_px(layout.ring_scale) / 2 - 22;
    clear_w = ring_left - clear_x;
    int max_w = layout.w - (clear_x - layout.x) - 16;
    if (clear_w < 140 || clear_w > max_w) clear_w = max_w;
  }

  int clear_y = layout.time_y - 10;
  int clear_h = (layout.time_scale > 1) ? 72 : 40;
  if (clear_y < layout.y) clear_y = layout.y;
  if (clear_x < layout.x) clear_x = layout.x;
  if (clear_h > layout.h - (clear_y - layout.y)) clear_h = layout.h - (clear_y - layout.y);
  gfx->fillRect(clear_x, clear_y, clear_w, clear_h, bg);
}

static int collect_active_timer_indices(int* activeIndices) {
  int count = 0;
  for (int i = 0; i < MAX_TIMERS && count < active_timer_count; i++) {
    if (timers[i].active) {
      activeIndices[count++] = i;
    }
  }
  return count;
}

static void redrawTimerArtRegions() {
  int activeIndices[MAX_TIMERS];
  int count = collect_active_timer_indices(activeIndices);

  for (int i = 0; i < count; i++) {
    int idx = activeIndices[i];
    const Timer& timer = timers[idx];
    PanelLayout layout = panel_layout_for(active_timer_count, i);
    TimerTheme theme = resolved_theme_for_timer(timer);
    bool redraw_this = (g_gif_dirty_theme_mask == 0);

    #if USE_GIFS
    if (!redraw_this && timer.theme_id < 32) {
      redraw_this = ((g_gif_dirty_theme_mask >> timer.theme_id) & 0x1U) != 0;
    }
    #endif

    if (!redraw_this) continue;

    if (layout.show_art) {
      gfx->fillRect(layout.art_box_x, layout.art_box_y, layout.art_box_w, layout.art_box_h, theme.bg);
      draw_panel_art_accents(layout, theme);
      draw_timer_art(layout, timer, theme);
    }
  }
}

static void redrawTimerTimeRegions() {
  int activeIndices[MAX_TIMERS];
  int count = collect_active_timer_indices(activeIndices);

  for (int i = 0; i < count; i++) {
    int idx = activeIndices[i];
    const Timer& timer = timers[idx];
    PanelLayout layout = panel_layout_for(active_timer_count, i);
    TimerTheme theme = resolved_theme_for_timer(timer);

    clear_timer_time_region(layout, theme.bg);
    char hhmmss[9];
    fmt_hhmmss(timer.seconds_left, hhmmss);
    draw_ui_text_layout(hhmmss, layout.time_anchor_x, layout.time_y,
                        layout.time_centered, theme.text, layout.time_scale);
  }
}

static void renderTimers();  // Forward declaration

// ======================= TIMING STATE =======================
static uint32_t last_second_ms = 0;
static uint32_t last_ring_ms = 0;
static uint32_t last_idle_anim_ms = 0;
static char last_text[MAX_TIMERS][9] = {"--------", "--------", "--------"};
static char last_name[MAX_TIMERS][16] = {"", "", ""};
static bool needs_full_redraw = true;
static bool needs_visual_redraw = false;
static bool needs_time_redraw = false;
static bool needs_art_redraw = false;
static bool full_redraw_resets_timebase = true;

static void requestFullRedraw() {
  needs_full_redraw = true;
  needs_visual_redraw = false;
  needs_time_redraw = false;
  needs_art_redraw = false;
  full_redraw_resets_timebase = true;
}

static void requestVisualRedraw() {
  needs_visual_redraw = true;
}

static void requestTimeRedraw() {
  needs_time_redraw = true;
}

static void requestArtRedraw() {
  needs_art_redraw = true;
}

// ======================= MAIN SETUP =======================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] Timer Device Starting...");

  // Initialize display
  Wire.setClock(1000000);
  gfx->begin();
  gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);
  
  Serial.println("[BOOT] Display initialized");

  #if USE_GIFS
  #if USE_LITTLEFS
  g_gif_fs_ready = LittleFS.begin(false);
  if (!g_gif_fs_ready && FS_FORMAT_ON_FAIL) {
    g_gif_fs_ready = LittleFS.begin(true);
  }
  #else
  g_gif_fs_ready = SPIFFS.begin(false);
  if (!g_gif_fs_ready && FS_FORMAT_ON_FAIL) {
    g_gif_fs_ready = SPIFFS.begin(true);
  }
  #endif

  if (g_gif_fs_ready) {
    Serial.printf("[GIF] Mounted %s\n", fsName());
  } else {
    Serial.printf("[GIF] %s mount failed, using fallback art\n", fsName());
  }
  #endif

  // Initialize timers
  for (int i = 0; i < MAX_TIMERS; i++) {
    sprintf(timers[i].name, "Timer %d", i + 1);
    timers[i].total_seconds = 0;
    timers[i].seconds_left = 0;
    timers[i].active = false;
    timers[i].ringing = false;
    timers[i].theme_id = THEME_DEFAULT;
  }

  #if USE_SPEAKER
  #if !USE_MP3_SFX
  // Initialize PWM audio output on A0
  // (Used for alarm + default SFX). We keep a reasonably high base freq.
  // NOTE: ESP32 Arduino core 3.x changed LEDC APIs; this matches speaker_test.ino.
  if (!ledcAttach(AUDIO_PIN, 10000, 8)) {
    Serial.println("[BOOT] Audio PWM attach failed");
  }
  ledcWriteTone(AUDIO_PIN, 0);
  ledcWrite(AUDIO_PIN, 0);
  #endif

  #if USE_MP3_SFX
  // Optional: MP3 SFX over I2S (only if your speaker amp is I2S-driven).
  g_out = new AudioOutputI2S();
  g_out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DOUT);
  g_out->SetGain(g_sfx_gain);
  #endif
  
  Serial.println("[BOOT] Audio initialized");
  
  // Play bootup sound
  sfx_play(SFX_POWER_ON);
  #endif

  #if USE_BLE
  // Initialize BLE
  NimBLEDevice::init("TimerDevice");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Nordic UART Service
  NimBLEService* pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  
  rxChar = pService->createCharacteristic(
    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  rxChar->setCallbacks(new CharacteristicCallbacks());
  
  txChar = pService->createCharacteristic(
    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
    NIMBLE_PROPERTY::NOTIFY
  );
  
  pService->start();
  
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(pService->getUUID());
  pAdvertising->start();
  
  Serial.println("[BOOT] BLE advertising started");
  #endif

  #if USE_RING
  ensure_ring_lut();
  #endif

  // Show initial screen
  drawNoTimersScreen(millis());
  gfx->flush();
  
  last_second_ms = millis();
  last_ring_ms = millis();
  last_idle_anim_ms = millis();
  
  Serial.println("[BOOT] Ready!");
  
  // Initialize demo mode
  if (INDEPENDENT_DEMO) {
    demo_start_ms = millis();
    Serial.println("[DEMO] Demo mode enabled - will simulate Nicla commands");
    Serial.printf("[DEMO] %d commands queued\n", DEMO_COMMAND_COUNT);
    for (int i = 0; i < DEMO_COMMAND_COUNT; i++) {
      Serial.printf("[DEMO]   @%lums: %s\n", 
                    (unsigned long)demo_command_queue[i].trigger_ms,
                    demo_command_queue[i].command);
      demo_command_queue[i].executed = false;  // Reset execution state
    }
  }
  
  needs_full_redraw = true;
}

// ======================= RENDER TIMERS =======================
static void renderTimers() {
  if (active_timer_count == 0) {
    drawNoTimersScreen(millis());
    return;
  }

  int activeIndices[MAX_TIMERS];
  int count = collect_active_timer_indices(activeIndices);
  gfx->fillScreen(COLOR_IDLE_BG);
  uint32_t now = millis();

  for (int i = 0; i < count; i++) {
    int idx = activeIndices[i];
    PanelLayout layout = panel_layout_for(active_timer_count, i);
    draw_timer_panel(layout, timers[idx], now);

    char hhmmss[9];
    fmt_hhmmss(timers[idx].seconds_left, hhmmss);
    strcpy(last_text[i], hhmmss);
    strcpy(last_name[i], timers[idx].name);
  }
}

// ======================= DEMO MODE PROCESSING =======================
static void processDemoCommands() {
  if (!INDEPENDENT_DEMO) return;
  
  uint32_t elapsed = millis() - demo_start_ms;
  
  for (int i = 0; i < DEMO_COMMAND_COUNT; i++) {
    if (!demo_command_queue[i].executed && elapsed >= demo_command_queue[i].trigger_ms) {
      demo_command_queue[i].executed = true;
      
      Serial.printf("[DEMO] Executing command: %s\n", demo_command_queue[i].command);
      
      // Parse and execute the command
      std::string cmdStr(demo_command_queue[i].command);
      ParsedCommand cmd = parseCommand(cmdStr);
      
      if (cmd.cmd != CMD_NONE) {
        processVoiceCommand(cmd);
        requestFullRedraw();
      }
    }
  }
}

// ======================= MAIN LOOP =======================
void loop() {
  uint32_t now = millis();
  
  #if USE_SPEAKER
  sfx_loop();  // Keep MP3 decoder running
  #endif
  
  // === Process demo commands if in demo mode ===
  processDemoCommands();
  
  // Check if active timer count changed (need full redraw)
  static int last_active_count = 0;
  if (active_timer_count != last_active_count) {
    last_active_count = active_timer_count;
    requestFullRedraw();
  }

  #if USE_GIFS
  if (sync_theme_gifs(now)) {
    requestArtRedraw();
  }
  #endif

  const uint32_t IDLE_FRAME_DT = 80;
  if (active_timer_count == 0 && !needs_full_redraw && now - last_idle_anim_ms >= IDLE_FRAME_DT) {
    last_idle_anim_ms = now;
    requestVisualRedraw();
  }
  
  // Full redraw if needed
  if (needs_full_redraw) {
    bool redraw_resets_timebase = full_redraw_resets_timebase;
    needs_full_redraw = false;
    needs_visual_redraw = false;
    needs_time_redraw = false;
    needs_art_redraw = false;
    full_redraw_resets_timebase = false;
    renderTimers();
    if (redraw_resets_timebase) {
      last_second_ms = now;
      last_ring_ms = now;
    }
  } else {
    if (needs_visual_redraw) {
      needs_visual_redraw = false;
      if (active_timer_count == 0) {
        redrawNoTimersBorder(now);
      }
    }

    if (active_timer_count > 0) {
      if (needs_art_redraw) {
        needs_art_redraw = false;
        redrawTimerArtRegions();
        g_gif_dirty_theme_mask = 0;
      }
      if (needs_time_redraw) {
        needs_time_redraw = false;
        redrawTimerTimeRegions();
      }
    } else {
      needs_art_redraw = false;
      needs_time_redraw = false;
    }
  }
  
  // === Update countdown once per second ===
  bool layout_changed = false;
  bool any_timer_changed = false;
  while (now - last_second_ms >= 1000) {
    last_second_ms += 1000;
    uint32_t tick_now = last_second_ms;

    for (int i = 0; i < MAX_TIMERS; i++) {
      if (!timers[i].active) continue;

      if (timers[i].ringing) {
        // Check for auto-shutoff.
        if (tick_now - timers[i].ring_start_ms >= ALARM_DURATION_MS) {
          timers[i].ringing = false;
          timers[i].active = false;
          updateActiveCount();
          layout_changed = true;
          #if USE_SPEAKER
          play_alarm_tone(false);
          #endif
        }
      } else if (timers[i].seconds_left > 0) {
        timers[i].seconds_left--;
        any_timer_changed = true;

        // Timer finished - start alarm.
        if (timers[i].seconds_left == 0) {
          timers[i].ringing = true;
          timers[i].ring_start_ms = tick_now;
          Serial.printf("[TIMER] %s finished! Alarm ringing.\n", timers[i].name);
        }
      }
    }
  }

  if (layout_changed) {
    requestFullRedraw();
  } else if (any_timer_changed && !needs_full_redraw) {
    requestTimeRedraw();
  }
  
  // === Update ring animation (~15 FPS) ===
  const uint32_t RING_FRAME_DT = 67;  // ~15 FPS
  if (now - last_ring_ms >= RING_FRAME_DT) {
    last_ring_ms = now;
    
    #if USE_RING
    if (active_timer_count > 0 && !needs_full_redraw) {
      int activeIndices[MAX_TIMERS];
      int count = 0;
      for (int i = 0; i < MAX_TIMERS && count < active_timer_count; i++) {
        if (timers[i].active) {
          activeIndices[count++] = i;
        }
      }
      
      // Sub-second interpolation for smooth ring animation
      float sub = (float)(now - last_second_ms) / 1000.0f;
      if (sub > 1) sub = 1;
      
      for (int i = 0; i < count; i++) {
        int idx = activeIndices[i];
        PanelLayout layout = panel_layout_for(active_timer_count, i);
        float remainingExact = (float)timers[idx].seconds_left + (1.0f - sub);
        if (remainingExact < 0) remainingExact = 0;
        float frac = 0.0f;
        if (timers[idx].total_seconds > 0) {
          frac = remainingExact / (float)timers[idx].total_seconds;
        }

        draw_timer_ring(layout, timers[idx], frac, now);
      }
    }
    #endif
  }

  // === Handle alarm sounds ===
  #if USE_SPEAKER
  bool any_ringing = false;
  for (int i = 0; i < MAX_TIMERS; i++) {
    if (timers[i].active && timers[i].ringing) {
      any_ringing = true;
      break;
    }
  }
  
  if (any_ringing && !sfx_playing) {
    // Use simple tone for alarm (more reliable than MP3 for continuous sound)
    play_alarm_tone(true);
  } else if (!any_ringing) {
    play_alarm_tone(false);
  }
  #endif
  
  // === Serial command interface for testing ===
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.startsWith("set ")) {
      // "set Timer 1 300" - create timer with 300 seconds
      int firstSpace = cmd.indexOf(' ', 4);
      if (firstSpace > 4) {
        String name = cmd.substring(4, firstSpace);
        uint32_t duration = cmd.substring(firstSpace + 1).toInt();
        createTimer(name.c_str(), duration);
        requestFullRedraw();
      }
    } else if (cmd.startsWith("cancel ")) {
      String name = cmd.substring(7);
      cancelTimer(name.c_str());
      requestFullRedraw();
    } else if (cmd == "stop") {
      stopAllAlarms();
      requestFullRedraw();
    } else if (cmd.startsWith("add ")) {
      // "add Timer 1 60"
      int firstSpace = cmd.indexOf(' ', 4);
      if (firstSpace > 4) {
        String name = cmd.substring(4, firstSpace);
        uint32_t duration = cmd.substring(firstSpace + 1).toInt();
        addTimeToTimer(name.c_str(), duration);
      }
    } else if (cmd == "status") {
      Serial.println("\n=== Timer Status ===");
      for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active) {
          char hhmmss[9];
          fmt_hhmmss(timers[i].seconds_left, hhmmss);
          Serial.printf("  [%d] %s: %s %s\n", i, timers[i].name, hhmmss,
                        timers[i].ringing ? "(RINGING)" : "");
        }
      }
      Serial.printf("Active: %d\n", active_timer_count);
    } else if (cmd == "help") {
      Serial.println("\n=== Commands ===");
      Serial.println("  set <name> <seconds> - Create timer");
      Serial.println("  cancel <name>        - Cancel timer");
      Serial.println("  add <name> <seconds> - Add time");
      Serial.println("  stop                 - Stop all alarms");
      Serial.println("  status               - Show timer status");
      Serial.println("  vol <0-100>          - Set SFX volume (PWM loudness; MP3 gain if enabled)\n");
    } else if (cmd.startsWith("vol ")) {
      int pct = cmd.substring(4).toInt();
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      g_sfx_gain = (float)pct / 100.0f;
      #if USE_SPEAKER && USE_MP3_SFX
      if (g_out) g_out->SetGain(g_sfx_gain);
      #endif

      // PWM path: map 0..100% to a safe duty range.
      // Cap at ~40% duty to avoid overdriving small amps/speakers.
      uint16_t duty = (uint16_t)((pct * 255) / 100);
      if (duty > 102) duty = 102;
      g_pwm_duty = (uint8_t)duty;
      Serial.printf("[SFX] Gain set to %.2f (%d%%)\n", g_sfx_gain, pct);
      Serial.printf("[SFX] PWM duty=%u  (and MP3 gain=%.2f if enabled)\n", (unsigned)g_pwm_duty, g_sfx_gain);
    }
  }
  
  delay(2);
}
