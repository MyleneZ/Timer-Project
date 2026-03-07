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
 *   - BLE communication with Nicla Voice for voice commands
 *   - Sound effects for feedback (bootup, confirm, cancel, alarm)
 *   - Voice commands: Set, Cancel, Add, Minus, Stop
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <ctype.h>
#include <math.h>
#include <vector>
#include <string>
#include "command_protocol.h"
#include "font/RacingSansOne_Regular20pt7b.h"
#include <NimBLEDevice.h>

// ======================= FEATURE FLAGS =======================
#define USE_MIC       0   // Disable local mic (using Nicla Voice instead)
#define USE_RING      1   // Enable ring animation
#define USE_SPEAKER   1   // Enable speaker output
#define USE_BLE       1   // Enable BLE for Nicla Voice communication

// If your speaker is driven by an analog/PWM input (common on STEMMA speaker amps),
// MP3->I2S output will sound awful. Default to PWM-based SFX.
#define USE_MP3_SFX   0   // 0 = PWM beep SFX (recommended), 1 = MP3 over I2S

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
  THEME_EXERCISE = 4
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
static const uint16_t COLOR_ALERT_RED = hex565(0xff738d);
static const uint16_t COLOR_ALERT_ORANGE = hex565(0xff9c5a);
static const uint16_t COLOR_ALERT_START = hex565(0xffeef2);

static const TimerTheme THEME_DEFAULT_STYLE = {
  hex565(0x24364a),
  hex565(0xe4f0ff),
  hex565(0x7fbcff),
  hex565(0x56718b),
  WHITE,
  hex565(0xd9e5f1),
  hex565(0x9cc3e4),
  hex565(0xffffff),
  hex565(0x33495f)
};

static const TimerTheme THEME_BREAK_STYLE = {
  hex565(0x4e3a3a),
  hex565(0xf0dcdf),
  hex565(0xd8b2b7),
  hex565(0x7a6466),
  WHITE,
  hex565(0xebe7e2),
  hex565(0xc7cccf),
  hex565(0x6c4320),
  hex565(0x988f88)
};

static const TimerTheme THEME_HOMEWORK_STYLE = {
  hex565(0x425c79),
  hex565(0xd8eeff),
  hex565(0x71bbff),
  hex565(0x6687a3),
  WHITE,
  hex565(0xf2b37d),
  hex565(0xdfb0cf),
  hex565(0x8f4d7e),
  hex565(0xbfd5ea)
};

static const TimerTheme THEME_BAKING_STYLE = {
  hex565(0x7f4259),
  hex565(0xffd7dd),
  hex565(0xff7d93),
  hex565(0xa8647b),
  WHITE,
  hex565(0xff7c67),
  hex565(0xffd8a6),
  hex565(0x5fc5c4),
  hex565(0xa85870)
};

static const TimerTheme THEME_EXERCISE_STYLE = {
  hex565(0x1f5b63),
  hex565(0xc4eee5),
  hex565(0x44d0ba),
  hex565(0x427f82),
  WHITE,
  hex565(0xb6e0d8),
  hex565(0x9dc8c1),
  hex565(0xdaf5ef),
  hex565(0x2e7479)
};

static uint16_t RANDOM_RING_COLORS[] = {
  hex565(0x71bbff),
  hex565(0xff7d93),
  hex565(0x44d0ba),
  hex565(0xc481ff),
  hex565(0xff9c5a)
};

static bool invertGradient = false;

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
  uint16_t ring_color;
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
  switch (theme_id) {
    case THEME_BREAK: return THEME_BREAK_STYLE;
    case THEME_HOMEWORK: return THEME_HOMEWORK_STYLE;
    case THEME_BAKING: return THEME_BAKING_STYLE;
    case THEME_EXERCISE: return THEME_EXERCISE_STYLE;
    default: return THEME_DEFAULT_STYLE;
  }
}

static TimerTheme resolved_theme_for_timer(const Timer& timer) {
  TimerTheme theme = theme_from_id(timer.theme_id);
  if (timer.theme_id == THEME_DEFAULT) {
    theme.ring_end = timer.ring_color;
    theme.ring_start = lerp565(hex565(0xfafcff), timer.ring_color, 84);
    theme.ring_empty = lerp565(theme.bg, timer.ring_color, 76);
  }
  return theme;
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

static uint16_t pick_random_color() {
  int colorPicker = random(0, (int)(sizeof(RANDOM_RING_COLORS) / sizeof(RANDOM_RING_COLORS[0])));
  return RANDOM_RING_COLORS[colorPicker];
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
  gfx->draw16bitRGBBitmap(x, y, ringbuf, SZ, SZ);
  gfx->endWrite();
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
  timers[slot].ring_color = pick_random_color();
  if (timers[slot].theme_id != THEME_DEFAULT) {
    timers[slot].ring_color = theme_from_id(timers[slot].theme_id).ring_end;
  }
  
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
  int title_x;
  int title_y;
  uint8_t title_scale;
  int time_x;
  int time_y;
  uint8_t time_scale;
  int art_x;
  int art_y;
  int art_size;
  int ring_cx;
  int ring_cy;
  float ring_scale;
};

static void configure_ui_font(uint8_t scale) {
  gfx->setFont(&RacingSansOne_Regular20pt7b);
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

static PanelLayout panel_layout_for(int active_count, int slot) {
  PanelLayout layout = {};
  layout.y = 0;
  layout.h = 320;

  if (active_count == 1) {
    layout.x = 0;
    layout.w = 960;
    layout.title_x = 46;
    layout.title_y = 42;
    layout.title_scale = 1;
    layout.time_x = 46;
    layout.time_y = 86;
    layout.time_scale = 2;
    layout.art_x = 164;
    layout.art_y = 220;
    layout.art_size = 120;
    layout.ring_cx = 728;
    layout.ring_cy = 160;
    layout.ring_scale = 0.96f;
  } else if (active_count == 2) {
    layout.x = slot * 480;
    layout.w = 480;
    layout.title_x = layout.x + 28;
    layout.title_y = 34;
    layout.title_scale = 1;
    layout.time_x = layout.x + 28;
    layout.time_y = 78;
    layout.time_scale = 1;
    layout.art_x = layout.x + 108;
    layout.art_y = 220;
    layout.art_size = 104;
    layout.ring_cx = layout.x + 344;
    layout.ring_cy = 164;
    layout.ring_scale = 0.80f;
  } else {
    layout.x = slot * 320;
    layout.w = 320;
    layout.title_x = layout.x + 18;
    layout.title_y = 22;
    layout.title_scale = 1;
    layout.time_x = layout.x + 18;
    layout.time_y = 56;
    layout.time_scale = 1;
    layout.art_x = layout.x + 82;
    layout.art_y = 224;
    layout.art_size = 72;
    layout.ring_cx = layout.x + 236;
    layout.ring_cy = 148;
    layout.ring_scale = 0.58f;
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
  gfx->fillCircle(cx, cy + size / 10, size / 3, theme.art_primary);
  gfx->fillCircle(cx, cy + size / 5, size / 4, theme.art_secondary);
  gfx->fillRect(cx - size / 4, cy - size / 12, size / 2, size / 9, theme.art_primary);
  gfx->drawLine(cx + size / 6, cy - size / 3, cx + size / 4, cy + size / 9, theme.art_tertiary);
  gfx->drawLine(cx + size / 12, cy - size / 4, cx + size / 4, cy + size / 11, theme.art_tertiary);
  gfx->drawLine(cx + size / 8, cy - size / 6, cx + size / 3, cy - size / 8, theme.art_tertiary);
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

static void draw_panel_backdrop(const PanelLayout& layout, const TimerTheme& theme) {
  gfx->fillCircle(layout.art_x, layout.art_y, layout.art_size / 2 + 12, theme.art_shadow);
  gfx->fillCircle(layout.art_x - layout.art_size / 3, layout.art_y - layout.art_size / 3,
                  layout.art_size / 8, lerp565(theme.ring_start, theme.art_primary, 90));
  gfx->fillCircle(layout.ring_cx, layout.ring_cy,
                  ring_size_px(layout.ring_scale) / 2 + 10,
                  lerp565(theme.bg, theme.ring_empty, 72));
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
  draw_theme_illustration(timer.theme_id, layout.art_x, layout.art_y, layout.art_size, theme);
  draw_ui_text(timer.name, layout.title_x, layout.title_y, theme.text, layout.title_scale);
  draw_ui_text(hhmmss, layout.time_x, layout.time_y, theme.text, layout.time_scale);

  float frac = 0.0f;
  if (timer.total_seconds > 0) {
    frac = (float)timer.seconds_left / (float)timer.total_seconds;
  }
  draw_timer_ring(layout, timer, frac, now);
}

static void drawNoTimersScreen() {
  gfx->fillScreen(COLOR_IDLE_BG);
  draw_ui_text_centered("No Active Timers", 480, 34, WHITE, 1);
  draw_ui_text_centered("Say 'Set a timer' to begin", 480, 90, COLOR_IDLE_SUB, 1);
  draw_ui_text_centered("or connect via Bluetooth", 480, 128, COLOR_IDLE_SUB, 1);

  const TimerTheme previews[] = {
    THEME_BREAK_STYLE,
    THEME_HOMEWORK_STYLE,
    THEME_BAKING_STYLE,
    THEME_EXERCISE_STYLE
  };
  const uint8_t previewIds[] = {
    THEME_BREAK,
    THEME_HOMEWORK,
    THEME_BAKING,
    THEME_EXERCISE
  };
  const int centers[] = {156, 364, 572, 780};

  for (int i = 0; i < 4; i++) {
    draw_theme_illustration(previewIds[i], centers[i] - 28, 236, 58, previews[i]);
    #if USE_RING
    ensure_ring_lut(0.38f);
    int ring_size = ring_size_px(0.38f);
    draw_ring(0.84f, CAP_LEAD, previews[i].ring_start, previews[i].ring_end, previews[i].ring_empty,
              centers[i] - ring_size / 2, 186, COLOR_IDLE_BG, 0.38f);
    #endif
  }
}

static void renderTimers();  // Forward declaration

// ======================= TIMING STATE =======================
static uint32_t last_second_ms = 0;
static uint32_t last_ring_ms = 0;
static char last_text[MAX_TIMERS][9] = {"--------", "--------", "--------"};
static char last_name[MAX_TIMERS][16] = {"", "", ""};
static bool needs_full_redraw = true;

static void requestFullRedraw() {
  needs_full_redraw = true;
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

  // Initialize timers
  for (int i = 0; i < MAX_TIMERS; i++) {
    sprintf(timers[i].name, "Timer %d", i + 1);
    timers[i].total_seconds = 0;
    timers[i].seconds_left = 0;
    timers[i].active = false;
    timers[i].ringing = false;
    timers[i].ring_color = RANDOM_RING_COLORS[0];
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
  drawNoTimersScreen();
  
  last_second_ms = millis();
  last_ring_ms = millis();
  
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
    drawNoTimersScreen();
    return;
  }

  // Get active timer indices
  int activeIndices[MAX_TIMERS];
  int count = 0;
  for (int i = 0; i < MAX_TIMERS && count < active_timer_count; i++) {
    if (timers[i].active) {
      activeIndices[count++] = i;
    }
  }
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
  
  // Full redraw if needed
  if (needs_full_redraw) {
    needs_full_redraw = false;
    renderTimers();
    last_second_ms = now;
    last_ring_ms = now;
  }
  
  // === Update countdown once per second ===
  if (now - last_second_ms >= 1000) {
    last_second_ms += 1000;
    
    bool any_timer_changed = false;
    
    for (int i = 0; i < MAX_TIMERS; i++) {
      if (!timers[i].active) continue;
      
      if (timers[i].ringing) {
        // Check for auto-shutoff
        if (now - timers[i].ring_start_ms >= ALARM_DURATION_MS) {
          timers[i].ringing = false;
          timers[i].active = false;
          updateActiveCount();
          requestFullRedraw();
          #if USE_SPEAKER
          play_alarm_tone(false);
          #endif
        }
      } else if (timers[i].seconds_left > 0) {
        timers[i].seconds_left--;
        any_timer_changed = true;
        
        // Timer finished - start alarm
        if (timers[i].seconds_left == 0) {
          timers[i].ringing = true;
          timers[i].ring_start_ms = now;
          Serial.printf("[TIMER] %s finished! Alarm ringing.\n", timers[i].name);
        }
      }
    }
    
    if (any_timer_changed && !needs_full_redraw) {
      requestFullRedraw();
    }
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
