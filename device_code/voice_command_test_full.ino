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

const int RING_SZ = 192;        // smaller = faster
const int RING_RO = 96;         // outer radius
const int RING_RI = 72;         // inner radius
const int RING_CY = 160;
const int RING_CX = 650;        // we went from 720 to 650, adjust text if needed
const int RING_X  = RING_CX - RING_SZ/2;
const int RING_Y  = RING_CY - RING_SZ/2;

const int TXT_Y = UI_RIGHT_Y - 32;
const int TXT_X = RING_CX - RING_RO - 412;
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

// auto_flush = true
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
static inline uint16_t hex565(uint32_t rgb) { // 0xRRGGBB
  return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}
static inline uint16_t lerp565(uint16_t c0, uint16_t c1, uint8_t t /*0..255*/) {
  uint16_t r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
  uint16_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint16_t r = (uint16_t)((r0 * (255 - t) + r1 * t) / 255);
  uint16_t g = (uint16_t)((g0 * (255 - t) + g1 * t) / 255);
  uint16_t b = (uint16_t)((b0 * (255 - t) + b1 * t) / 255);
  return (r << 11) | (g << 5) | b;
}
static uint16_t GRAD_START = hex565(0xe9faff);
static uint16_t GRAD_END   = hex565(0x0099ff);
static uint16_t GRAD_BLUE   = hex565(0x0099ff);
static uint16_t GRAD_RED    = hex565(0xFE809F);
static uint16_t GRAD_GREEN  = hex565(0x59CEB9);
static uint16_t GRAD_PURPLE = hex565(0xC481FF);
static uint16_t GRAD_ORANGE = hex565(0xFF845B);
static bool     invertGradient = false;

static void draw_grid_left(uint16_t color, int dx, int dy) {
  for (int x = 0; x < UI_RIGHT_Y; x += dx) gfx->drawFastVLine(x, 0, gfx->height(), color);
  for (int y = 0; y < gfx->height(); y += dy) gfx->drawFastHLine(0, y, UI_RIGHT_Y, color);
}
static void fmt_hhmmss(uint32_t sec, char *out, uint16_t color) {
  gfx->setTextColor(WHITE, color);
  uint32_t m = sec / 60, s = sec % 60;
  uint32_t h = m / 60; m = m % 60;
  sprintf(out, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

// ===================== VOICE: TOKENS / IDs =====================
enum TokenId : uint8_t {
  TK_SET, TK_CANCEL, TK_ADD, TK_MINUS, TK_STOP, TK_TIMER,
  TK_MINUTE, TK_MINUTES, TK_HOUR, TK_HOURS,
  TK_ONE, TK_TWO, TK_THREE, TK_FOUR, TK_FIVE, TK_SIX, TK_SEVEN, TK_EIGHT, TK_NINE,
  TK_TEN, TK_ELEVEN, TK_TWELVE, TK_THIRTEEN, TK_FOURTEEN, TK_FIFTEEN, TK_SIXTEEN, TK_SEVENTEEN, TK_EIGHTEEN, TK_NINETEEN,
  TK_TWENTY, TK_THIRTY, TK_FORTY, TK_FIFTY, TK_SIXTY, TK_SEVENTY, TK_EIGHTY, TK_NINETY,
  TK_BAKING, TK_COOKING, TK_BREAK, TK_HOMEWORK, TK_EXERCISE, TK_WORKOUT,
  TK__COUNT
};

static const char* KWS_CMD_NAMES[TK__COUNT] = {
  "set","cancel","add","minus","stop","timer",
  "minute","minutes","hour","hours",
  "one","two","three","four","five","six","seven","eight","nine",
  "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen",
  "twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety",
  "baking","cooking","break","homework","exercise","workout"
};

#if USE_MIC
// ======== MIC sample convert (matches your current use) ========
static inline int16_t sph0645_word_to_s16(int32_t w32) {
  int32_t s18 = (w32 >> 14);
  return (int16_t)(s18 >> 2);
}
#endif

// ===================== TIMERS =====================
static const uint32_t COUNTDOWN_SECONDS_1 = 10 * 60;
static const uint32_t COUNTDOWN_SECONDS_2 = 5 * 60;
static const uint32_t COUNTDOWN_SECONDS_3 = 5;
int timer_array[3] = {COUNTDOWN_SECONDS_1, COUNTDOWN_SECONDS_2, COUNTDOWN_SECONDS_3};

static char timer_name_1[16] = "Timer 1";
static uint32_t countdown_left = timer_array[0];
static uint32_t last_second_ms = 0;
static uint32_t last_ring_ms = 0;
static char last_text[9] = "--------";
static char last_name[16] = "---------------";

static char timer_name_2[16] = "Timer 2";
static uint32_t countdown_left_2 = timer_array[1];
static uint32_t last_second_ms_2 = 0;
static uint32_t last_ring_ms_2 = 0;
static char last_text_2[9] = "--------";
static char last_name_2[16] = "---------------";

static char timer_name_3[16] = "Timer 3";
static uint32_t countdown_left_3 = timer_array[2];
static uint32_t last_second_ms_3 = 0;
static uint32_t last_ring_ms_3 = 0;
static char last_text_3[9] = "--------";
static char last_name_3[16] = "---------------";

// ===================== RING (unchanged visuals) =====================
#if USE_RING
static uint16_t ringbuf[RING_SZ * RING_SZ];
static uint16_t angleLUT[RING_SZ * RING_SZ];
static uint8_t  maskLUT[RING_SZ * RING_SZ];

static inline int ring_size_px(float scale) {
  int sz = (int)(RING_SZ * scale + 0.5f);
  return sz < 1 ? 1 : sz;
}
static void init_ring_lut(float scale=1.0f) {
  int colorPicker = random(0,5);
  switch(colorPicker) {
    case 0: GRAD_END = GRAD_BLUE; break;
    case 1: GRAD_END = GRAD_RED; break;
    case 2: GRAD_END = GRAD_GREEN; break;
    case 3: GRAD_END = GRAD_PURPLE; break;
    case 4: GRAD_END = GRAD_ORANGE; break;
  }
  const int SZ  = ring_size_px(scale);
  const float SCALE = 65535.0f / TWO_PI;
  const int cx = SZ / 2;
  const int cy = SZ / 2;

  // scale radii
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
enum CapMode : uint8_t { CAP_NONE=0, CAP_LEAD=1, CAP_TRAIL=2, CAP_BOTH=3 };
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

  const float ro   = RING_RO * scale;
  const float ri   = RING_RI * scale;
  const float thick= ro - ri;
  const float r_mid= 0.5f * (ro + ri);
  const float cap_r= 0.5f * thick + 0.5f;
  const float cap_r2 = cap_r * cap_r;

  const float a_lead  = (1.0f - fracRemaining) * TWO_PI;
  const float a_trail = 0.0f;

  float capX[2], capY[2]; uint8_t nCaps = 0;
  if (caps & CAP_LEAD)  { capX[nCaps] = cx + r_mid * sinf(a_lead);  capY[nCaps] = cy - r_mid * cosf(a_lead);  ++nCaps; }
  if (caps & CAP_TRAIL) { capX[nCaps] = cx + r_mid * sinf(a_trail); capY[nCaps] = cy - r_mid * cosf(a_trail); ++nCaps; }

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

  gfx->startWrite();
  gfx->draw16bitRGBBitmap(x, y, ringbuf, SZ, SZ);
  gfx->endWrite();
}
#endif
// -------------------------------------------------------------

// ===================== RENDER TIMERS =====================
void renderTimers() {
  int active_timers = 0;
  for (int i=0; i<3; i++) if (timer_array[i] > 0) active_timers++;

  gfx->fillScreen(hex565(0x14215E));
  if (active_timers == 1) {
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);
    gfx->setTextSize(8); gfx->setTextWrap(false); gfx->setTextColor(WHITE, hex565(0x14215E));
    #if USE_RING
      init_ring_lut();
      draw_ring(1.0f, CAP_LEAD);
    #endif
    last_second_ms = millis(); last_ring_ms = millis();
    char hhmmss[9]; fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss); gfx->setCursor(TXT_X, TXT_Y); gfx->print(hhmmss);
    strcpy(last_name, timer_name_1); gfx->setTextSize(4); gfx->setCursor(TXT_X, TXT_Y - 40); gfx->print(timer_name_1);

  } else if (active_timers == 2) {
    gfx->fillRect(480, 0, 480, 320, hex565(0x2139A4));
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);
    gfx->setTextSize(5); gfx->setTextWrap(false); gfx->setTextColor(WHITE, hex565(0x14215E));
    #if USE_RING
      init_ring_lut();
      draw_ring(1.0f, CAP_LEAD, RING_CX - 680, RING_CY - 60);
      draw_ring(1.0f, CAP_LEAD, RING_CX + 480 - 680, RING_CY - 60, hex565(0x2139A4));
    #endif
    last_second_ms = millis(); last_ring_ms = millis();
    char hhmmss[9]; fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss); gfx->setCursor(TXT_X + 85, TXT_Y - 80); gfx->print(hhmmss);
    strcpy(last_name, timer_name_1); gfx->setTextSize(4); gfx->setCursor(TXT_X + 85, TXT_Y - 120); gfx->print(timer_name_1);

    gfx->setTextSize(5);
    fmt_hhmmss(countdown_left_2, hhmmss, hex565(0x2139A4));
    strcpy(last_text_2, hhmmss); gfx->setCursor(TXT_X + 565, TXT_Y - 80); gfx->print(hhmmss);
    strcpy(last_name_2, timer_name_2); gfx->setTextSize(4); gfx->setCursor(TXT_X + 565, TXT_Y - 120); gfx->print(timer_name_2);

  } else if (active_timers == 3) {
    gfx->fillRect(320, 0, 320, 320, hex565(0x2139A4));
    gfx->fillRect(640, 0, 320, 320, hex565(0x3F56C0));
    gfx->drawRect(0, 0, VU_W, gfx->height(), DARKGREY);
    gfx->setTextSize(4); gfx->setTextWrap(false); gfx->setTextColor(WHITE, hex565(0x14215E));
    #if USE_RING
      init_ring_lut(0.8f);
      draw_ring(1.0f, CAP_LEAD, RING_CX - 650, RING_CY - 30, hex565(0x14215E), 0.8f);
      draw_ring(1.0f, CAP_LEAD, RING_CX + 320 - 650, RING_CY - 30, hex565(0x2139A4), 0.8f);
      draw_ring(1.0f, CAP_LEAD, RING_CX + 640 - 650, RING_CY - 30, hex565(0x3F56C0), 0.8f);
    #endif
    last_second_ms = millis(); last_ring_ms = millis();
    char hhmmss[9]; fmt_hhmmss(countdown_left, hhmmss, hex565(0x14215E));
    strcpy(last_text, hhmmss); gfx->setCursor(TXT_X - 100, TXT_Y - 80); gfx->print(hhmmss);
    strcpy(last_name, timer_name_1); gfx->setTextSize(3); gfx->setCursor(TXT_X - 100, TXT_Y - 120); gfx->print(timer_name_1);

    gfx->setTextSize(4);
    fmt_hhmmss(countdown_left_2, hhmmss, hex565(0x2139A4));
    strcpy(last_text_2, hhmmss); gfx->setCursor(TXT_X + 230, TXT_Y - 80); gfx->print(hhmmss);
    strcpy(last_name_2, timer_name_2); gfx->setTextSize(3); gfx->setCursor(TXT_X + 230, TXT_Y - 120); gfx->print(timer_name_2);

    gfx->setTextSize(4);
    fmt_hhmmss(countdown_left_3, hhmmss, hex565(0x3F56C0));
    strcpy(last_text_3, hhmmss); gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 80); gfx->print(hhmmss);
    strcpy(last_name_3, timer_name_3); gfx->setTextSize(3); gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 120); gfx->print(timer_name_3);
  }
}

// ===================== TINY KEYWORD SPOTTER =====================
//
// How to use (Serial @115200):
//   enroll <id>      -> records one template for token id (see KWS_CMD_NAMES)
// Say sequences like: "add ... five ... minutes ... timer ... one"
// Prints tokens + parsed intent. No timer actions executed yet.
//

#if USE_MIC
// ---- Parameters ----
#define KWS_SR               16000
#define KWS_FRAME_SAMPLES      400   // 25ms
#define KWS_HOP_SAMPLES        160   // 10ms
#define KWS_RING_SAMPLES    (KWS_SR * 2) // 2s audio ring

#define KWS_NBINS               24
#define KWS_MAX_FRAMES         120   // ~1.2s
#define KWS_MAX_TEMPLATES        3   // per token (kept small to fit RAM)

// DTW & VAD
#define KWS_DTW_BAND            10
#define KWS_VAD_RMS_ON       900.0f
#define KWS_VAD_RMS_OFF      500.0f
#define KWS_VAD_ZC_RATIO       0.02f
#define KWS_VAD_MIN_FRAMES       15
#define KWS_VAD_POSTROLL_FR       8

// Accept thresholds
#define KWS_ACCEPT_THRESH     1500.0f
#define KWS_MARGIN_MIN         150.0f

// ---- Audio ring (int16) ----
static volatile int16_t g_audio_ring[KWS_RING_SAMPLES];
static volatile uint32_t g_wr_idx = 0;

static inline int16_t dc_block(int16_t x) {
  static int16_t xprev=0; static float yprev=0;
  float y = float(x - xprev) + 0.995f * yprev;
  xprev = x; yprev = y;
  if (y > 32767) y = 32767; if (y < -32768) y = -32768;
  return (int16_t)y;
}
static void kws_push_audio_s16(const int16_t* s, size_t n) {
  for (size_t i=0;i<n;++i) {
    int16_t v = dc_block(s[i]);
    g_audio_ring[g_wr_idx] = v;
    g_wr_idx = (g_wr_idx + 1) % KWS_RING_SAMPLES;
  }
}

// ---- Goertzel bank 300..4000 Hz ----
static float g_goertzel_w[KWS_NBINS];
static void kws_init_goertzel() {
  const float fmin=300.0f, fmax=4000.0f;
  for (int b=0; b<KWS_NBINS; ++b) {
    float r=(float)b/(KWS_NBINS-1);
    float fc=fmin*powf(fmax/fmin,r);
    float k=(KWS_FRAME_SAMPLES*fc)/(float)KWS_SR;
    g_goertzel_w[b]=2.0f*cosf(2.0f*PI*k/(float)KWS_FRAME_SAMPLES);
  }
}
static void kws_goertzel_frame(const int16_t* x, float* out24) {
  static float hann[KWS_FRAME_SAMPLES]; static bool inited=false;
  if(!inited){ for(int n=0;n<KWS_FRAME_SAMPLES;++n) hann[n]=0.5f-0.5f*cosf(2.0f*PI*n/(KWS_FRAME_SAMPLES-1)); inited=true; }
  for(int b=0;b<KWS_NBINS;++b){
    float s_prev=0,s_prev2=0,w=g_goertzel_w[b];
    for(int n=0;n<KWS_FRAME_SAMPLES;++n){
      float s = hann[n]*(float)x[n] + w*s_prev - s_prev2;
      s_prev2=s_prev; s_prev=s;
    }
    float power = s_prev2*s_prev2 + s_prev*s_prev - w*s_prev*s_prev2;
    out24[b] = logf(1e-3f + power);
  }
}
static bool kws_vad_frame(const int16_t* x) {
  int64_t acc=0; int zc=0; int16_t prev=x[0];
  for(int i=0;i<KWS_FRAME_SAMPLES;++i){ int16_t s=x[i]; acc+=(int32_t)s*(int32_t)s; if((s^prev)<0) zc++; prev=s; }
  float rms = sqrtf((float)acc/(float)KWS_FRAME_SAMPLES);
  return (rms > KWS_VAD_RMS_ON && zc > (int)(KWS_FRAME_SAMPLES*KWS_VAD_ZC_RATIO));
}

typedef struct { uint16_t T; float feats[KWS_MAX_FRAMES][KWS_NBINS]; } kws_feats_t;

// ---- Template storage with dynamic feat buffers to save RAM ----
struct KwsTemplate { uint16_t T=0; float* feats=nullptr; };
struct KwsBank { uint8_t n=0; KwsTemplate tpl[KWS_MAX_TEMPLATES]; } ;
static KwsBank g_bank[TK__COUNT];

static void tpl_free(KwsTemplate& t){ if(t.feats){ free(t.feats); t.feats=nullptr; } t.T=0; }

// DTW
static float kws_dtw_l2(const float* A,uint16_t TA,const float* B,uint16_t TB,uint16_t D,uint16_t band){
  const float INF=1e30f;
  static float dp[KWS_MAX_FRAMES+1][KWS_MAX_FRAMES+1];
  for(int i=0;i<=TA;i++) for(int j=0;j<=TB;j++) dp[i][j]=INF; dp[0][0]=0.0f;
  for(uint16_t i=1;i<=TA;++i){
    int jmin=max<int>(1,i-band), jmax=min<int>(TB,i+band);
    for(int j=jmin;j<=jmax;++j){
      float cost=0.0f; const float* a=A+(i-1)*D; const float* b=B+(j-1)*D;
      for(uint16_t d=0; d<D; ++d){ float diff=a[d]-b[d]; cost+=diff*diff; }
      float m=dp[i-1][j]; if(dp[i][j-1]<m) m=dp[i][j-1]; if(dp[i-1][j-1]<m) m=dp[i-1][j-1];
      dp[i][j]=cost+m;
    }
  }
  float raw=dp[TA][TB]; return raw/(float)(TA+TB);
}

// Capture one utterance using ring + VAD + CMVN
static bool kws_capture_utterance(kws_feats_t* out) {
  static int16_t frame[KWS_FRAME_SAMPLES];
  uint32_t rd = (g_wr_idx + KWS_RING_SAMPLES - (KWS_FRAME_SAMPLES + 10)) % KWS_RING_SAMPLES;
  uint16_t frames=0; bool inSpeech=false; int inCount=0,outCount=0;
  const uint32_t maxHops = (KWS_SR * 2) / KWS_HOP_SAMPLES;
  static float tmp[KWS_MAX_FRAMES][KWS_NBINS];

  static float m[KWS_NBINS], s[KWS_NBINS];

  for (uint32_t hop=0; hop<maxHops; ++hop) {
    uint32_t fstart = (rd + KWS_RING_SAMPLES - KWS_FRAME_SAMPLES) % KWS_RING_SAMPLES;
    for (int n=0;n<KWS_FRAME_SAMPLES;++n) frame[n]=g_audio_ring[(fstart+n)%KWS_RING_SAMPLES];
    bool speech = kws_vad_frame(frame);
    rd = (rd + KWS_HOP_SAMPLES) % KWS_RING_SAMPLES;

    if (!inSpeech) { if (speech) { if(++inCount>=2) inSpeech=true; } else inCount=0; }
    else { if (!speech) { if(++outCount>=KWS_VAD_POSTROLL_FR) break; } else outCount=0; }

    if (inSpeech && frames < KWS_MAX_FRAMES) { kws_goertzel_frame(frame, tmp[frames]); frames++; }
  }
  if (frames < KWS_VAD_MIN_FRAMES) return false;

  // CMVN
  for(int d=0; d<KWS_NBINS; ++d){ m[d]=0; s[d]=0; }
  for(int t=0; t<frames; ++t) for(int d=0; d<KWS_NBINS; ++d) m[d]+=tmp[t][d];
  for(int d=0; d<KWS_NBINS; ++d) m[d]/=frames;
  for(int t=0; t<frames; ++t) for(int d=0; d<KWS_NBINS; ++d){ float z=tmp[t][d]-m[d]; s[d]+=z*z; }
  for(int d=0; d<KWS_NBINS; ++d){ s[d]=sqrtf(s[d]/(float)max(1,frames-1)); if(s[d]<1e-6f) s[d]=1.0f; }
  out->T = frames;
  for(int t=0; t<frames; ++t) for(int d=0; d<KWS_NBINS; ++d) out->feats[t][d] = (tmp[t][d]-m[d])/s[d];
  return true;
}

// Enrollment
static bool g_enrolling=false; static int g_enroll_id=-1;
static void kws_start_enroll(int id){
  if(id<0 || id>=TK__COUNT){ Serial.println("Bad id 0..(TK__COUNT-1)"); return; }
  g_enrolling=true; g_enroll_id=id;
  Serial.print("Listening to enroll id="); Serial.print(id);
  Serial.print(" -> \""); Serial.print(KWS_CMD_NAMES[id]); Serial.println("\"");
}
static void kws_finish_enroll(const kws_feats_t& utt){
  KwsBank& bk = g_bank[g_enroll_id];
  int slot = (bk.n < KWS_MAX_TEMPLATES) ? bk.n++ : 0; // replace round-robin when full
  // free old
  tpl_free(bk.tpl[slot]);
  // alloc new
  size_t bytes = (size_t)utt.T * KWS_NBINS * sizeof(float);
  float* buf = (float*) malloc(bytes);
  if(!buf){ Serial.println("Enroll OOM"); g_enrolling=false; g_enroll_id=-1; return; }
  for(int t=0;t<utt.T;++t) for(int d=0;d<KWS_NBINS;++d) buf[t*KWS_NBINS + d] = utt.feats[t][d];
  bk.tpl[slot].T = utt.T; bk.tpl[slot].feats = buf;

  Serial.print("Enrolled id="); Serial.print(g_enroll_id);
  Serial.print(" (T="); Serial.print(utt.T); Serial.print(", slot="); Serial.print(slot); Serial.println(")");
  g_enrolling=false; g_enroll_id=-1;
}

// Token ring + tiny grammar
struct HeardToken{ TokenId id; uint32_t t_ms; };
static HeardToken g_tokring[16]; static uint8_t g_tokhead=0;
static void push_token(TokenId id){ g_tokring[g_tokhead] = { id, millis() }; g_tokhead = (g_tokhead + 1) % 16; }

static inline bool is_number(TokenId tk){
  return (tk>=TK_ONE && tk<=TK_NINETEEN) || (tk>=TK_TWENTY && tk<=TK_NINETY);
}
static int token_to_number(TokenId tk){
  switch(tk){
    case TK_ONE:return 1; case TK_TWO:return 2; case TK_THREE:return 3; case TK_FOUR:return 4; case TK_FIVE:return 5;
    case TK_SIX:return 6; case TK_SEVEN:return 7; case TK_EIGHT:return 8; case TK_NINE:return 9; case TK_TEN:return 10;
    case TK_ELEVEN:return 11; case TK_TWELVE:return 12; case TK_THIRTEEN:return 13; case TK_FOURTEEN:return 14; case TK_FIFTEEN:return 15;
    case TK_SIXTEEN:return 16; case TK_SEVENTEEN:return 17; case TK_EIGHTEEN:return 18; case TK_NINETEEN:return 19;
    case TK_TWENTY:return 20; case TK_THIRTY:return 30; case TK_FORTY:return 40; case TK_FIFTY:return 50;
    case TK_SIXTY:return 60; case TK_SEVENTY:return 70; case TK_EIGHTY:return 80; case TK_NINETY:return 90;
    default:return -1;
  }
}
static int parse_number_from_window(int startIdx,int dir,int look=3){
  int found[2]={-1,-1}; int fcnt=0; int idx=startIdx;
  for(int k=0;k<look;++k,idx+=dir){
    uint8_t pos=(uint8_t)((idx+16)%16);
    TokenId tk=g_tokring[pos].id;
    if(is_number(tk)){
      found[fcnt++]=token_to_number(tk);
      if(fcnt==2) break;
    }else if(fcnt>0){ break; }
  }
  if(fcnt==0) return -1; if(fcnt==1) return found[0];
  if(found[0]>=20 || found[1]>=20) return found[0]+found[1];
  return found[0];
}
enum IntentKind { INTENT_NONE, INTENT_STOP, INTENT_CANCEL, INTENT_ADJUST, INTENT_SET };
struct Intent { IntentKind kind=INTENT_NONE; int timer_idx=-1; int seconds=0; };

// Units
static int unit_to_seconds(TokenId tk){
  if(tk==TK_MINUTE||tk==TK_MINUTES) return 60;
  if(tk==TK_HOUR||tk==TK_HOURS) return 3600;
  return 0;
}
static void on_intent(const Intent& it){
  Serial.print("[INTENT] kind=");
  switch(it.kind){
    case INTENT_STOP: Serial.print("STOP"); break;
    case INTENT_CANCEL: Serial.print("CANCEL"); break;
    case INTENT_ADJUST: Serial.print("ADJUST"); break;
    case INTENT_SET: Serial.print("SET"); break;
    default: Serial.print("NONE"); break;
  }
  Serial.print(" timer="); Serial.print(it.timer_idx);
  Serial.print(" seconds="); Serial.println(it.seconds);
}
static void try_parse_and_emit_intent(){
  const uint32_t NOW=millis(); const uint32_t WINDOW_MS=2200;
  auto at=[&](int rel)->HeardToken&{ uint8_t pos=(uint8_t)((g_tokhead+rel+16)%16); return g_tokring[pos]; };
  auto fresh_is=[&](int rel,TokenId tk)->bool{ HeardToken &h=at(rel); return (h.id==tk && (NOW-h.t_ms)<=WINDOW_MS); };

  if(fresh_is(-1,TK_STOP)){ on_intent({INTENT_STOP,-1,0}); return; }

  for(int rel=-1; rel>=-8; --rel){
    HeardToken &h=at(rel); if((NOW-h.t_ms)>WINDOW_MS) break;

    if(h.id==TK_CANCEL){
      if(fresh_is(rel-1,TK_TIMER)){
        int tnum = parse_number_from_window(rel-2,-1);
        if(tnum>0){ on_intent({INTENT_CANCEL,tnum,0}); return; }
      }
    }

    if(h.id==TK_ADD || h.id==TK_MINUS){
      int sign=(h.id==TK_ADD)?+1:-1; int qty=-1, unit=0, timerN=-1;
      for(int a=1;a<=6;++a){
        TokenId tk=at(rel - a).id;
        if(qty<0 && is_number(tk)) qty = parse_number_from_window(rel - a,+1);
        if(unit==0){ int u=unit_to_seconds(tk); if(u) unit=u; }
        if(timerN<0 && tk==TK_TIMER) timerN = parse_number_from_window(rel - a - 1,+1);
      }
      if(qty>0 && unit>0 && timerN>0){
        on_intent({INTENT_ADJUST,timerN,sign*qty*unit}); return;
      }
    }

    if(h.id==TK_SET){
      int timerN=-1,qty=-1,unit=0;
      for(int a=1;a<=8;++a){
        TokenId tk=at(rel - a).id;
        if(tk==TK_TIMER) timerN = parse_number_from_window(rel - a - 1,+1);
        else if(is_number(tk) && qty<0) qty = parse_number_from_window(rel - a,+1);
        else { int u=unit_to_seconds(tk); if(u) unit=u; }
      }
      if(timerN>0 && qty>0 && unit>0){
        on_intent({INTENT_SET,timerN,qty*unit}); return;
      }
    }
  }
}
static void on_command_detected(int cmdId){
  Serial.print("KWS TOKEN: "); Serial.print(KWS_CMD_NAMES[cmdId]); Serial.print(" (id="); Serial.print(cmdId); Serial.println(")");
  push_token((TokenId)cmdId);
  try_parse_and_emit_intent();
}
static void kws_try_recognize(const kws_feats_t& utt){
  float best=1e30f,second=1e30f; int bestId=-1;
  for(int id=0; id<TK__COUNT; ++id){
    KwsBank& bk=g_bank[id]; if(bk.n==0) continue;
    float bestCmd=1e30f;
    for(int t=0;t<bk.n;++t){
      if(!bk.tpl[t].feats || bk.tpl[t].T==0) continue;
      float d = kws_dtw_l2(&utt.feats[0][0],utt.T,bk.tpl[t].feats,bk.tpl[t].T,KWS_NBINS,KWS_DTW_BAND);
      if(d<bestCmd) bestCmd=d;
    }
    if(bestCmd<best){ second=best; best=bestCmd; bestId=id; }
    else if(bestCmd<second){ second=bestCmd; }
  }
  if(bestId>=0 && best<KWS_ACCEPT_THRESH && (second-best)>KWS_MARGIN_MIN){
    on_command_detected(bestId);
  }
}
static void kws_init(){
  kws_init_goertzel();
  Serial.println("[KWS] Ready. Use: enroll <id>  (see token table)");
}
static void kws_process(){
  // Handle Serial commands
  if(Serial.available()){
    String s=Serial.readStringUntil('\n'); s.trim();
    if(s.startsWith("enroll")){
      int sp=s.indexOf(' ');
      if(sp>0){ int id=s.substring(sp+1).toInt(); kws_start_enroll(id); }
      else Serial.println("Usage: enroll <id>");
    }
  }
  // Capture & either enroll or recognize
  kws_feats_t utt;
  if(!kws_capture_utterance(&utt)) return;
  if(g_enrolling) kws_finish_enroll(utt); else kws_try_recognize(utt);
}
#endif // USE_MIC

// =============================== SETUP / LOOP ===============================
void setup() {
  Wire.setClock(1000000);
  gfx->begin(); gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

#if USE_MIC
  Serial.begin(115200);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // LEFT works, RIGHT VU=0
    .communication_format = I2S_COMM_FORMAT_STAND_I2S, // keep your setting
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

  renderTimers();

#if USE_MIC
  kws_init();
#endif
}

void loop() {
  uint32_t now = millis();

  // Count active timers
  int active_timers = 0; for (int i=0;i<3;i++) if (timer_array[i] > 0) active_timers++;

  // --- Update countdown text once per second ---
  if (now - last_second_ms >= 1000) {
    last_second_ms += 1000;

    char hhmmss1[9], hhmmss2[9], hhmmss3[9];

    if (active_timers >= 1) {
      countdown_left = (countdown_left > 0) ? (countdown_left - 1) : COUNTDOWN_SECONDS_1;
      fmt_hhmmss(countdown_left, hhmmss1, hex565(0x14215E));
    }
    if (active_timers >= 2) {
      countdown_left_2 = (countdown_left_2 > 0) ? (countdown_left_2 - 1) : COUNTDOWN_SECONDS_2;
      fmt_hhmmss(countdown_left_2, hhmmss2, hex565(0x2139A4));
    }
    if (active_timers >= 3) {
      countdown_left_3 = (countdown_left_3 > 0) ? (countdown_left_3 - 1) : COUNTDOWN_SECONDS_3;
      fmt_hhmmss(countdown_left_3, hhmmss3, hex565(0x3F56C0));
    }

    if (active_timers == 1) {
      if (strcmp(hhmmss1, last_text) != 0) {
        strcpy(last_text, hhmmss1);
        gfx->setTextColor(WHITE, hex565(0x14215E));
        gfx->setTextSize(8); gfx->setCursor(TXT_X, TXT_Y); gfx->print(hhmmss1);
      }
      if (strcmp(timer_name_1, last_name) != 0) {
        strcpy(last_name, timer_name_1);
        gfx->setTextColor(WHITE, hex565(0x14215E)); gfx->setTextSize(4);
        gfx->setCursor(TXT_X - 60, TXT_Y - 40); gfx->print(timer_name_1);
      }
    } else if (active_timers == 2) {
      if (strcmp(hhmmss1, last_text) != 0) {
        strcpy(last_text, hhmmss1);
        gfx->setTextColor(WHITE, hex565(0x14215E));
        gfx->setTextSize(5); gfx->setCursor(TXT_X + 85, TXT_Y - 80); gfx->print(hhmmss1);
      }
      if (strcmp(timer_name_1, last_name) != 0) {
        strcpy(last_name, timer_name_1);
        gfx->setTextColor(WHITE, hex565(0x14215E)); gfx->setTextSize(4);
        gfx->setCursor(TXT_X + 85, TXT_Y - 120); gfx->print(timer_name_1);
      }
      if (strcmp(hhmmss2, last_text_2) != 0) {
        strcpy(last_text_2, hhmmss2);
        gfx->setTextColor(WHITE, hex565(0x2139A4));
        gfx->setTextSize(5); gfx->setCursor(TXT_X + 565, TXT_Y - 80); gfx->print(hhmmss2);
      }
      if (strcmp(timer_name_2, last_name_2) != 0) {
        strcpy(last_name_2, timer_name_2);
        gfx->setTextColor(WHITE, hex565(0x2139A4)); gfx->setTextSize(4);
        gfx->setCursor(TXT_X + 565, TXT_Y - 120); gfx->print(timer_name_2);
      }
    } else if (active_timers == 3) {
      if (strcmp(hhmmss1, last_text) != 0) {
        strcpy(last_text, hhmmss1);
        gfx->setTextColor(WHITE, hex565(0x14215E));
        gfx->setTextSize(4); gfx->setCursor(TXT_X - 100, TXT_Y - 80); gfx->print(hhmmss1);
      }
      if (strcmp(timer_name_1, last_name) != 0) {
        strcpy(last_name, timer_name_1);
        gfx->setTextColor(WHITE, hex565(0x14215E));
        gfx->setTextSize(3); gfx->setCursor(TXT_X - 100, TXT_Y - 120); gfx->print(timer_name_1);
      }
      if (strcmp(hhmmss2, last_text_2) != 0) {
        strcpy(last_text_2, hhmmss2);
        gfx->setTextColor(WHITE, hex565(0x2139A4));
        gfx->setTextSize(4); gfx->setCursor(TXT_X + 230, TXT_Y - 80); gfx->print(hhmmss2);
      }
      if (strcmp(timer_name_2, last_name_2) != 0) {
        strcpy(last_name_2, timer_name_2);
        gfx->setTextColor(WHITE, hex565(0x2139A4));
        gfx->setTextSize(3); gfx->setCursor(TXT_X + 230, TXT_Y - 120); gfx->print(timer_name_2);
      }
      if (strcmp(hhmmss3, last_text_3) != 0) {
        strcpy(last_text_3, hhmmss3);
        gfx->setTextColor(WHITE, hex565(0x3F56C0));
        gfx->setTextSize(4); gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 80); gfx->print(hhmmss3);
      }
      if (strcmp(timer_name_3, last_name_3) != 0) {
        strcpy(last_name_3, timer_name_3);
        gfx->setTextColor(WHITE, hex565(0x3F56C0));
        gfx->setTextSize(3); gfx->setCursor(TXT_X + 465 + 85, TXT_Y - 120); gfx->print(timer_name_3);
      }
    }
  }

  // --- Separately update rings for smoother animation ---
  const uint32_t FRAME_DT = 67;
  if (now - last_ring_ms >= FRAME_DT) {
    do { last_ring_ms +=  FRAME_DT; } while (now - last_ring_ms >= FRAME_DT);

    #if USE_RING
      float frac1=1.0f, frac2=1.0f, frac3=1.0f;
      float sub = (float)(now - last_ring_ms) / 1000.0f; if (sub<0) sub=0; if (sub>1) sub=1;

      if (active_timers >= 1) {
        float remainingExact = (float)countdown_left + (0.067f - sub);
        if (remainingExact < 0) remainingExact = 0;
        frac1 = remainingExact / (float)COUNTDOWN_SECONDS_1;
      }
      if (active_timers >= 2) {
        float remainingExact2 = (float)countdown_left_2 + (0.067f - sub);
        if (remainingExact2 < 0) remainingExact2 = 0;
        frac2 = remainingExact2 / (float)COUNTDOWN_SECONDS_2;
      }
      if (active_timers >= 3) {
        float remainingExact3 = (float)countdown_left_3 + (0.067f - sub);
        if (remainingExact3 < 0) remainingExact3 = 0;
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

#if USE_MIC
  // === MIC: read, VU, and feed KWS ===
  static uint32_t last_audio_print = 0;
  int32_t sampleBuf32[256]; size_t readBytes = 0;
  if (i2s_read(I2S_PORT, sampleBuf32, sizeof(sampleBuf32), &readBytes, 0) == ESP_OK && readBytes) {
    const size_t n = readBytes / sizeof(int32_t);
    static int16_t s16buf[256];
    int32_t abs_accum = 0;

    for (size_t i = 0; i < n; ++i) {
      int16_t s = sph0645_word_to_s16(sampleBuf32[i]);
      s16buf[i] = s;
      abs_accum += (s >= 0 ? s : -s);
    }

    // Push to KWS (after DC block inside kws_push_audio_s16)
    kws_push_audio_s16(s16buf, n);

    if (now - last_audio_print >= 100) {
      last_audio_print = now;
      if (n) {
        int32_t avg = abs_accum / (int32_t)n;
        Serial.printf("VU=%ld (n=%u)\n", (long)avg, (unsigned)n);
      }
    }
  }

  // Run KWS (handles enroll + recognize + intent parsing)
  kws_process();
#endif

  delay(2);
}
