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
#include <math.h>
#include <vector>
#include <string>
#include "command_protocol.h"
#include <NimBLEDevice.h>

// ======================= FEATURE FLAGS =======================
#define USE_MIC       0   // Disable local mic (using Nicla Voice instead)
#define USE_RING      1   // Enable ring animation
#define USE_SPEAKER   1   // Enable speaker output
#define USE_BLE       1   // Enable BLE for Nicla Voice communication

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

// Gradient colors
static uint16_t GRAD_START = hex565(0xe9faff);
static uint16_t GRAD_END   = hex565(0x0099ff);
static uint16_t GRAD_BLUE   = hex565(0x0099ff);
static uint16_t GRAD_RED    = hex565(0xFE809F);
static uint16_t GRAD_GREEN  = hex565(0x59CEB9);
static uint16_t GRAD_PURPLE = hex565(0xC481FF);
static uint16_t GRAD_ORANGE = hex565(0xFF845B);
static bool invertGradient = false;

// Background colors for different timer panels
static const uint16_t BG_PANEL_1 = hex565(0x14215E);
static const uint16_t BG_PANEL_2 = hex565(0x2139A4);
static const uint16_t BG_PANEL_3 = hex565(0x3F56C0);

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
};

static Timer timers[MAX_TIMERS];
static int active_timer_count = 0;

// ======================= RING BUFFER =======================
#if USE_RING
static uint16_t ringbuf[RING_SZ * RING_SZ];
static uint16_t angleLUT[RING_SZ * RING_SZ];
static uint8_t  maskLUT[RING_SZ * RING_SZ];

static inline int ring_size_px(float scale) {
  int sz = (int)(RING_SZ * scale + 0.5f);
  return sz < 1 ? 1 : sz;
}

static uint16_t pick_random_color() {
  int colorPicker = random(0, 5);
  switch(colorPicker) {
    case 0: return GRAD_BLUE;
    case 1: return GRAD_RED;
    case 2: return GRAD_GREEN;
    case 3: return GRAD_PURPLE;
    case 4: return GRAD_ORANGE;
    default: return GRAD_BLUE;
  }
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

enum CapMode : uint8_t {
  CAP_NONE  = 0,
  CAP_LEAD  = 1,
  CAP_TRAIL = 2,
  CAP_BOTH  = 3
};

static void draw_ring(float fracRemaining, uint8_t caps, uint16_t grad_end,
                      int x = RING_X, int y = RING_Y,
                      uint16_t bg = BG_PANEL_1, float scale = 1.0f) {
  if (fracRemaining < 0) fracRemaining = 0;
  if (fracRemaining > 1) fracRemaining = 1;

  const uint16_t threshold = (uint16_t)(fracRemaining * 65535.0f + 0.5f);
  const uint16_t cut = (uint16_t)(65535 - threshold);
  uint32_t span = (uint32_t)65535 - (uint32_t)cut;
  if (span == 0) span = 1;

  const uint16_t OUTSIDE = bg;
  const uint16_t BG_RING = hex565(0x44598C);

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
      uint16_t color = BG_RING;

      if (inArc) {
        uint8_t t = (uint8_t)(((uint32_t)(a - cut) * 255U) / span);
        if (invertGradient) t = 255 - t;
        color = lerp565(GRAD_START, grad_end, t);
      }

      if (!inArc && nCaps) {
        for (uint8_t i = 0; i < nCaps; ++i) {
          const float dx = (float)xx - capX[i];
          const float dy = (float)yy - capY[i];
          if (dx*dx + dy*dy <= cap_r2) {
            color = (i == 0 && (caps & CAP_LEAD)) ? GRAD_START : grad_end;
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

static void sfx_stop() {
  if (g_mp3) {
    if (g_mp3->isRunning()) g_mp3->stop();
    delete g_mp3;
    g_mp3 = nullptr;
  }
  if (g_src) {
    delete g_src;
    g_src = nullptr;
  }
  sfx_playing = false;
}

static void sfx_play(int idx) {
  if (SFX_MP3_COUNT == 0) return;
  if (idx < 0 || idx >= (int)SFX_MP3_COUNT) return;

  sfx_stop();
  g_mp3 = new AudioGeneratorMP3();
  g_src = new AudioFileSourcePROGMEM(SFX_MP3_LIST[idx].data, SFX_MP3_LIST[idx].len);
  
  if (g_mp3->begin(g_src, g_out)) {
    sfx_playing = true;
    Serial.printf("[SFX] Playing: %s\n", SFX_MP3_LIST[idx].name);
  }
}

static void sfx_loop() {
  if (g_mp3 && g_mp3->isRunning()) {
    if (!g_mp3->loop()) {
      g_mp3->stop();
      sfx_playing = false;
    }
  }
}

// Simple alarm tone using PWM (backup if MP3 fails)
static void play_alarm_tone(bool enable) {
  // Use ledcWriteTone for simple beeping when no MP3 available
  static bool alarm_state = false;
  static uint32_t last_toggle = 0;
  
  if (!enable) {
    ledcWriteTone(A0, 0);
    alarm_state = false;
    return;
  }
  
  uint32_t now = millis();
  if (now - last_toggle > 500) {
    last_toggle = now;
    alarm_state = !alarm_state;
    ledcWriteTone(A0, alarm_state ? 880 : 0);  // 880Hz beep
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
  timers[slot].ring_color = pick_random_color();
  
  updateActiveCount();
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

static void drawNoTimersScreen() {
  gfx->fillScreen(BG_PANEL_1);
  
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE, BG_PANEL_1);
  gfx->setTextWrap(false);
  
  // Center text on screen
  const char* line1 = "No Active Timers";
  const char* line2 = "Say 'Set a timer' or";
  const char* line3 = "connect via Bluetooth";
  
  gfx->setCursor(320 - strlen(line1) * 9, 120);
  gfx->print(line1);
  
  gfx->setTextSize(2);
  gfx->setCursor(320 - strlen(line2) * 6, 180);
  gfx->print(line2);
  gfx->setCursor(320 - strlen(line3) * 6, 210);
  gfx->print(line3);
  
  // Draw decorative rings
  #if USE_RING
  init_ring_lut(0.5f);
  draw_ring(1.0f, CAP_LEAD, GRAD_BLUE, 480 - 48, 160 - 48, BG_PANEL_1, 0.5f);
  #endif
}

static void renderTimers();  // Forward declaration

// ======================= TIMING STATE =======================
static uint32_t last_second_ms = 0;
static uint32_t last_ring_ms = 0;
static char last_text[MAX_TIMERS][9] = {"--------", "--------", "--------"};
static char last_name[MAX_TIMERS][16] = {"", "", ""};
static bool needs_full_redraw = true;

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
    timers[i].ring_color = GRAD_BLUE;
  }

  #if USE_SPEAKER
  // Initialize I2S audio output
  g_out = new AudioOutputI2S();
  g_out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DOUT);
  g_out->SetGain(0.5f);
  
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
  init_ring_lut();
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

  if (active_timer_count == 1) {
    // Single timer - full screen
    int idx = activeIndices[0];
    gfx->fillScreen(BG_PANEL_1);
    
    gfx->setTextSize(8);
    gfx->setTextWrap(false);
    gfx->setTextColor(WHITE, BG_PANEL_1);
    
    #if USE_RING
    init_ring_lut();
    float frac = (float)timers[idx].seconds_left / (float)timers[idx].total_seconds;
    draw_ring(frac, CAP_LEAD, timers[idx].ring_color, RING_X, RING_Y, BG_PANEL_1, 1.0f);
    #endif
    
    char hhmmss[9];
    fmt_hhmmss(timers[idx].seconds_left, hhmmss);
    strcpy(last_text[0], hhmmss);
    gfx->setCursor(TXT_X, TXT_Y);
    gfx->print(hhmmss);
    
    gfx->setTextSize(4);
    strcpy(last_name[0], timers[idx].name);
    gfx->setCursor(TXT_X, TXT_Y - 40);
    gfx->print(timers[idx].name);
    
  } else if (active_timer_count == 2) {
    // Two timers - split screen
    gfx->fillScreen(BG_PANEL_1);
    gfx->fillRect(480, 0, 480, 320, BG_PANEL_2);
    
    gfx->setTextSize(5);
    gfx->setTextWrap(false);
    
    #if USE_RING
    init_ring_lut(0.8f);
    #endif
    
    for (int i = 0; i < 2; i++) {
      int idx = activeIndices[i];
      uint16_t bg = (i == 0) ? BG_PANEL_1 : BG_PANEL_2;
      int xOffset = i * 480;
      
      gfx->setTextColor(WHITE, bg);
      
      #if USE_RING
      float frac = (float)timers[idx].seconds_left / (float)timers[idx].total_seconds;
      draw_ring(frac, CAP_LEAD, timers[idx].ring_color, 
                RING_CX - 680 + xOffset, RING_CY - 76, bg, 0.8f);
      #endif
      
      char hhmmss[9];
      fmt_hhmmss(timers[idx].seconds_left, hhmmss);
      strcpy(last_text[i], hhmmss);
      gfx->setCursor(TXT_X + 85 + xOffset, TXT_Y - 80);
      gfx->print(hhmmss);
      
      gfx->setTextSize(3);
      strcpy(last_name[i], timers[idx].name);
      gfx->setCursor(TXT_X + 85 + xOffset, TXT_Y - 120);
      gfx->print(timers[idx].name);
      gfx->setTextSize(5);
    }
    
  } else if (active_timer_count == 3) {
    // Three timers - thirds
    gfx->fillScreen(BG_PANEL_1);
    gfx->fillRect(320, 0, 320, 320, BG_PANEL_2);
    gfx->fillRect(640, 0, 320, 320, BG_PANEL_3);
    
    gfx->setTextSize(4);
    gfx->setTextWrap(false);
    
    #if USE_RING
    init_ring_lut(0.6f);
    #endif
    
    for (int i = 0; i < 3; i++) {
      int idx = activeIndices[i];
      uint16_t bg = (i == 0) ? BG_PANEL_1 : (i == 1) ? BG_PANEL_2 : BG_PANEL_3;
      int xOffset = i * 320;
      
      gfx->setTextColor(WHITE, bg);
      
      #if USE_RING
      float frac = (float)timers[idx].seconds_left / (float)timers[idx].total_seconds;
      int ringX = 160 + xOffset - (int)(RING_SZ * 0.6f / 2);
      int ringY = 80;
      draw_ring(frac, CAP_LEAD, timers[idx].ring_color, ringX, ringY, bg, 0.6f);
      #endif
      
      char hhmmss[9];
      fmt_hhmmss(timers[idx].seconds_left, hhmmss);
      strcpy(last_text[i], hhmmss);
      gfx->setCursor(60 + xOffset, 220);
      gfx->print(hhmmss);
      
      gfx->setTextSize(2);
      strcpy(last_name[i], timers[idx].name);
      gfx->setCursor(60 + xOffset, 190);
      gfx->print(timers[idx].name);
      gfx->setTextSize(4);
    }
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
        needs_full_redraw = true;
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
    needs_full_redraw = true;
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
          needs_full_redraw = true;
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
    
    // Update text display (partial update)
    if (any_timer_changed && !needs_full_redraw) {
      int activeIndices[MAX_TIMERS];
      int count = 0;
      for (int i = 0; i < MAX_TIMERS && count < active_timer_count; i++) {
        if (timers[i].active) {
          activeIndices[count++] = i;
        }
      }
      
      for (int i = 0; i < count; i++) {
        int idx = activeIndices[i];
        char hhmmss[9];
        fmt_hhmmss(timers[idx].seconds_left, hhmmss);
        
        if (strcmp(hhmmss, last_text[i]) != 0) {
          strcpy(last_text[i], hhmmss);
          
          // Determine position based on layout
          uint16_t bg;
          int textX, textY;
          int textSize;
          
          if (active_timer_count == 1) {
            bg = BG_PANEL_1;
            textX = TXT_X;
            textY = TXT_Y;
            textSize = 8;
          } else if (active_timer_count == 2) {
            bg = (i == 0) ? BG_PANEL_1 : BG_PANEL_2;
            textX = TXT_X + 85 + i * 480;
            textY = TXT_Y - 80;
            textSize = 5;
          } else {
            bg = (i == 0) ? BG_PANEL_1 : (i == 1) ? BG_PANEL_2 : BG_PANEL_3;
            textX = 60 + i * 320;
            textY = 220;
            textSize = 4;
          }
          
          gfx->setTextSize(textSize);
          gfx->setTextColor(WHITE, bg);
          gfx->setCursor(textX, textY);
          gfx->print(hhmmss);
        }
      }
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
        
        // Flashing effect when ringing
        if (timers[idx].ringing) {
          bool flash = ((now / 250) % 2) == 0;
          uint16_t ringColor = flash ? GRAD_RED : GRAD_ORANGE;
          
          // Draw full ring with flash
          float scale = (active_timer_count == 1) ? 1.0f : 
                        (active_timer_count == 2) ? 0.8f : 0.6f;
          
          uint16_t bg = (active_timer_count == 1) ? BG_PANEL_1 :
                        (i == 0) ? BG_PANEL_1 : (i == 1) ? BG_PANEL_2 : BG_PANEL_3;
          
          int ringX, ringY;
          if (active_timer_count == 1) {
            ringX = RING_X; ringY = RING_Y;
          } else if (active_timer_count == 2) {
            ringX = RING_CX - 680 + i * 480; ringY = RING_CY - 76;
          } else {
            ringX = 160 + i * 320 - (int)(RING_SZ * 0.6f / 2); ringY = 80;
          }
          
          init_ring_lut(scale);
          draw_ring(flash ? 0.0f : 0.1f, CAP_LEAD, ringColor, ringX, ringY, bg, scale);
          
        } else if (timers[idx].seconds_left > 0) {
          // Normal countdown ring
          float remainingExact = (float)timers[idx].seconds_left + (1.0f - sub);
          if (remainingExact < 0) remainingExact = 0;
          float frac = remainingExact / (float)timers[idx].total_seconds;
          
          float scale = (active_timer_count == 1) ? 1.0f : 
                        (active_timer_count == 2) ? 0.8f : 0.6f;
          
          uint16_t bg = (active_timer_count == 1) ? BG_PANEL_1 :
                        (i == 0) ? BG_PANEL_1 : (i == 1) ? BG_PANEL_2 : BG_PANEL_3;
          
          int ringX, ringY;
          if (active_timer_count == 1) {
            ringX = RING_X; ringY = RING_Y;
          } else if (active_timer_count == 2) {
            ringX = RING_CX - 680 + i * 480; ringY = RING_CY - 76;
          } else {
            ringX = 160 + i * 320 - (int)(RING_SZ * 0.6f / 2); ringY = 80;
          }
          
          init_ring_lut(scale);
          draw_ring(frac, CAP_LEAD, timers[idx].ring_color, ringX, ringY, bg, scale);
        }
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
        needs_full_redraw = true;
      }
    } else if (cmd.startsWith("cancel ")) {
      String name = cmd.substring(7);
      cancelTimer(name.c_str());
      needs_full_redraw = true;
    } else if (cmd == "stop") {
      stopAllAlarms();
      needs_full_redraw = true;
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
    }
  }
  
  delay(2);
}
