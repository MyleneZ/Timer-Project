#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <driver/i2s.h>

// ===================== Pins / Qualia =====================
#define I2S_PORT      I2S_NUM_0
#define PIN_BCLK      SCK     // Qualia SCK header pin
#define PIN_WS        A0      // LRCLK
#define PIN_SD        A1      // DATA

// ======= QUALIA DISPLAY WIRING (same as your project) =======
Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
  PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK, PCA_TFT_MOSI, &Wire, 0x3F);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
  TFT_R1, TFT_R2, TFT_R3, TFT_R4, TFT_R5,
  TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
  TFT_B1, TFT_B2, TFT_B3, TFT_B4, TFT_B5,
  1, 24, 4, 64,
  1, 12, 2, 20,
  1, 12000000
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  320, 960, rgbpanel, 0, true,
  expander, GFX_NOT_DEFINED,
  HD458002C40_init_operations, sizeof(HD458002C40_init_operations),
  80
);

// ===================== UI colors =====================
const uint16_t COL_BG = 0x1043;
const uint16_t COL_FG = 0xFFFF;
const uint16_t COL_BAR = 0x07E0;    // green
const uint16_t COL_WARN = 0xFD20;   // orange
const uint16_t COL_ERR = 0xF800;    // red

// ===================== Audio / VAD/KWS params =====================
#define SR_HZ          16000

// Keep these in sync with the template generator + firmware KWS core
#define WIN_MS         25
#define HOP_MS         10
#define FRAME_SAMPLES  (SR_HZ * WIN_MS / 1000)   // 400
#define HOP_SAMPLES    (SR_HZ * HOP_MS / 1000)   // 160

#define KWS_NBINS      24      // must match header
#define VAD_MIN_MS     220
#define VAD_HANG_MS    180
#define VAD_PREROLL_MS 180

// ---- Adaptive VAD (auto-cal + tracking) ----
static float noise_floor = 800.0f;          // estimated ambient (EMA)
static float vad_thresh  = 1600.0f;         // speech threshold
static const float TH_MULT = 1.8f;          // thresh = floor * TH_MULT
static const float FLOOR_ALPHA_IDLE = 0.01f;   // below threshold
static const float FLOOR_ALPHA_SPK  = 0.002f;  // above threshold (slow)

// ---- Simple 1-pole high-pass (DC/rumble cut ~100 Hz @16k) ----
typedef struct { float z; float x1; } HPF;
static HPF g_hpf;
static inline void hpf_init(HPF* f){ f->z = 0.0f; f->x1 = 0.0f; }
// y[n] = x[n] - x[n-1] + 0.995*y[n-1]
static inline float hpf_step(HPF* f, float x){
  float y = (x - f->x1) + 0.995f * f->z;
  f->z = y; f->x1 = x;
  return y;
}

// ===================== KWS templates =====================
#include "kws_templates.h"
KwsBank g_bank[KWS_TOKEN_COUNT];

static const char* TOKEN_NAME[KWS_TOKEN_COUNT] = {
  "set","cancel","add","minus","stop","timer",
  "minute","minutes","hour","hours",
  "one","two","three","four","five","six","seven","eight","nine",
  "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen","seventeen","eighteen","nineteen",
  "twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety",
  "baking","cooking","break","homework","exercise","workout"
};

// ===================== I2S init & helpers =====================
static bool useRight = false;  // runtime toggle: false=LEFT, true=RIGHT

static void i2s_init_16k_32bit(bool rightChannel)
{
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SR_HZ,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = rightChannel ? I2S_CHANNEL_FMT_ONLY_RIGHT
                                   : I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S, // if silent, try I2S_COMM_FORMAT_STAND_MSB
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false
  };
  i2s_pin_config_t pins = {
    .bck_io_num = PIN_BCLK,
    .ws_io_num = PIN_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_SD
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
}

static inline int16_t sph0645_word_to_s16(int32_t w32) {
  int32_t s18 = (w32 >> 14);
  return (int16_t)(s18 >> 2);
}

static size_t i2s_read_block(int16_t *out_s16, size_t max_samples_s16)
{
  int32_t tmp32[256];
  size_t bytes_read = 0;
  i2s_read(I2S_PORT, tmp32, sizeof(tmp32), &bytes_read, 0);
  size_t n32 = bytes_read / sizeof(int32_t);
  size_t n = (n32 > max_samples_s16) ? max_samples_s16 : n32;
  for (size_t i = 0; i < n; ++i) out_s16[i] = sph0645_word_to_s16(tmp32[i]);
  return n;
}

// ===================== DSP (24-band Goertzel, matches generator) =====================
static float goertzel_w[KWS_NBINS];
static float hann_win[FRAME_SAMPLES];

static void init_filterbank() {
  const float fmin = 300.0f;
  const float fmax = 4000.0f;
  for (int n = 0; n < FRAME_SAMPLES; ++n) {
    hann_win[n] = 0.5f - 0.5f * cosf(2.0f * PI * n / (FRAME_SAMPLES - 1));
  }
  for (int b = 0; b < KWS_NBINS; ++b) {
    float r = (float)b / (float)(KWS_NBINS - 1);
    float fc = fmin * powf(fmax / fmin, r);
    float k = (FRAME_SAMPLES * fc) / (float)SR_HZ;
    goertzel_w[b] = 2.0f * cosf(2.0f * PI * k / (float)FRAME_SAMPLES);
  }
}

static void frame_feats_24(const int16_t* in, int ofs, float out[KWS_NBINS]) {
  static float wf[FRAME_SAMPLES];
  float mean = 0.f;
  for (int i = 0; i < FRAME_SAMPLES; ++i) mean += in[ofs + i];
  mean /= FRAME_SAMPLES;
  for (int i = 0; i < FRAME_SAMPLES; ++i) wf[i] = ((float)in[ofs + i] - mean) * hann_win[i];
  for (int b = 0; b < KWS_NBINS; ++b) {
    float s_prev = 0.0f;
    float s_prev2 = 0.0f;
    float w = goertzel_w[b];
    for (int i = 0; i < FRAME_SAMPLES; ++i) {
      float s = wf[i] + w * s_prev - s_prev2;
      s_prev2 = s_prev;
      s_prev = s;
    }
    float power = s_prev2*s_prev2 + s_prev*s_prev - w*s_prev*s_prev2;
    out[b] = logf(1e-3f + power);
  }
}

static void znorm_frames(float* F, int T) {
  for (int b = 0; b < KWS_NBINS; ++b) {
    float mu=0; for (int t=0;t<T;++t) mu += F[t*KWS_NBINS+b]; mu /= T;
    float s2=0; for (int t=0;t<T;++t){ float d=F[t*KWS_NBINS+b]-mu; s2 += d*d; }
    float sd = sqrtf(s2/T + 1e-9f);
    for (int t=0;t<T;++t) F[t*KWS_NBINS+b] = (F[t*KWS_NBINS+b]-mu)/sd;
  }
}

// DTW on CMVN-normalized features (smaller = better)
static float dtw_l2(const float* A, int TA, const float* B, int TB, int band) {
  const float INF = 1e30f;
  static float dp[121][121];
  if (TA > 120) TA = 120;
  if (TB > 120) TB = 120;
  for (int i = 0; i <= TA; ++i) for (int j = 0; j <= TB; ++j) dp[i][j] = INF;
  dp[0][0] = 0.0f;
  for (int i = 1; i <= TA; ++i) {
    int jmin = max(1, i - band);
    int jmax = min(TB, i + band);
    for (int j = jmin; j <= jmax; ++j) {
      float cost = 0.0f;
      const float* a = A + (i - 1) * KWS_NBINS;
      const float* b = B + (j - 1) * KWS_NBINS;
      for (int d = 0; d < KWS_NBINS; ++d) {
        float diff = a[d] - b[d];
        cost += diff * diff;
      }
      float m = dp[i - 1][j];
      if (dp[i][j - 1] < m) m = dp[i][j - 1];
      if (dp[i - 1][j - 1] < m) m = dp[i - 1][j - 1];
      dp[i][j] = cost + m;
    }
  }
  return dp[TA][TB] / (float)(TA + TB);
}

// ===================== VU / Debug UI =====================
static void show_header(const char* l1, const char* l2) {
  gfx->fillScreen(COL_BG);
  gfx->setTextWrap(false);
  gfx->setTextColor(COL_FG, COL_BG);
  gfx->setTextSize(4);
  gfx->setCursor(20, 40);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(20, 80);
  gfx->print(l2);
}

static int vuW = 40, vuH = 200, vuX = 20, vuY = 120;
static int lastVuH = -1;

static void draw_vu_frame() {
  gfx->drawRect(vuX-2, vuY-2, vuW+4, vuH+4, COL_FG);
}

static void dbg_draw(float rms, float floorv, float thr, bool rightCh, bool vad_on){
  // bar
  int h = (int)(rms / 20.0f); if (h<0) h=0; if (h>vuH) h=vuH;
  if (h != lastVuH) {
    gfx->fillRect(vuX, vuY, vuW, vuH, COL_BG);
    uint16_t c = (rms > thr) ? COL_WARN : COL_BAR;
    gfx->fillRect(vuX, vuY + (vuH - h), vuW, h, c);
    lastVuH = h;
  }
  // text
  gfx->setTextColor(COL_FG, COL_BG);
  gfx->setTextSize(2);
  gfx->fillRect(80, 120, 230, 100, COL_BG);
  gfx->setCursor(80, 120); gfx->printf("RMS: %.0f\n", rms);
  gfx->setCursor(80, 140); gfx->printf("Floor: %.0f\n", floorv);
  gfx->setCursor(80, 160); gfx->printf("Thresh: %.0f\n", thr);
  gfx->setCursor(80, 180); gfx->printf("Chan: %s\n", rightCh ? "RIGHT" : "LEFT");
  gfx->setCursor(80, 200); gfx->printf("VAD: %s   ", vad_on ? "ON" : "off");
}

// ===================== KWS display =====================
static void show_detect(const char* word, float sim) {
  gfx->fillRect(20, 230, 280, 60, COL_BG);
  gfx->setTextColor(COL_FG, COL_BG);
  gfx->setTextSize(3);
  gfx->setCursor(20, 230);
  gfx->printf("Detected: %s", word ? word : "-");
  gfx->setTextSize(2);
  gfx->setCursor(20, 260);
  gfx->printf("dist=%.1f", sim);
}

// ===================== Matching helper =====================
static void match_and_display(const int16_t* segment, int nsamp) {
  const int T_max = 120;
  static float feats[T_max * KWS_NBINS];
  int T = 0;
  for (int ofs = 0; ofs + FRAME_SAMPLES <= nsamp && T < T_max; ofs += HOP_SAMPLES) {
    frame_feats_24(segment, ofs, &feats[T*KWS_NBINS]);
    ++T;
  }
  if (T < 3) return;
  znorm_frames(feats, T);

  float best = 1e30f, second = 1e30f;
  int bestTid = -1;
  const int band = 10;

  for (int tid = 0; tid < (int)KWS_TOKEN_COUNT; ++tid) {
    KwsBank &bk = g_bank[tid];
    if (bk.n == 0) continue;
    float bestForToken = 1e30f;
    for (int j = 0; j < bk.n; ++j) {
      if (!bk.tpl[j].feats || bk.tpl[j].T == 0) continue;
      float d = dtw_l2(feats, T, bk.tpl[j].feats, bk.tpl[j].T, band);
      if (d < bestForToken) bestForToken = d;
    }
    if (bestForToken < best) { second = best; best = bestForToken; bestTid = tid; }
    else if (bestForToken < second) { second = bestForToken; }
  }

  const float THRESH = 250.0f;
  const float MARGIN = 30.0f;
  bool accept = (bestTid >= 0) && (best < THRESH) && ((second - best) > MARGIN);
  if (accept) {
    show_detect(TOKEN_NAME[bestTid], best);
    Serial.printf("[KWS] %s  dist=%.2f  second=%.2f\n",
                  TOKEN_NAME[bestTid] ? TOKEN_NAME[bestTid] : "(unknown)", best, second);
  } else {
    show_detect("(none)", best);
    Serial.printf("[KWS] Reject. best=%d(%s) dist=%.2f second=%.2f\n",
      bestTid, (bestTid>=0 && TOKEN_NAME[bestTid])?TOKEN_NAME[bestTid]:"-", best, second);
  }
}

// ===================== Capture + adaptive VAD =====================
#define RING_SEC   2
#define RING_NSAMP (SR_HZ * RING_SEC)
static int16_t pcm_ring[RING_NSAMP];
static volatile uint32_t ring_w = 0;

void setup() {
  Wire.setClock(1000000);
  Serial.begin(115200);

  gfx->begin();
  gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  show_header("KWS + VU Test", "Press any key in Serial to toggle L/R");
  draw_vu_frame();

  i2s_init_16k_32bit(useRight);
  init_filterbank();
  kws_load_from_progmem();
  hpf_init(&g_hpf);

  // 500ms quick calibration for floor
  {
    uint32_t t0 = millis();
    double acc = 0.0; uint32_t nacc = 0;
    int16_t blk[256];
    while (millis() - t0 < 500) {
      size_t n = i2s_read_block(blk, 256);
      for (size_t i=0;i<n;++i){
        float s = hpf_step(&g_hpf, (float)blk[i]);
        acc += fabsf(s);
        nacc++;
      }
    }
    noise_floor = (nacc ? (float)(acc / nacc) : 800.0f);
    vad_thresh  = max(400.0f, noise_floor * TH_MULT);
  }
  Serial.printf("Calib: floor=%.1f  thresh=%.1f  (ambient ~%.0f)\n",
                noise_floor, vad_thresh, noise_floor);
}

void loop() {
  // Toggle channel when any byte arrives in Serial
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    i2s_driver_uninstall(I2S_PORT);
    useRight = !useRight;
    i2s_init_16k_32bit(useRight);
    lastVuH = -1;
  }

  // Read audio
  int16_t s16[256];
  size_t n = i2s_read_block(s16, 256);

  // Ring buffer + compute HPF-filtered RMS
  double e = 0.0;
  for (size_t i=0;i<n;++i){
    float s = hpf_step(&g_hpf, (float)s16[i]);
    e += (double)s * (double)s;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    pcm_ring[ring_w] = (int16_t)s;
    ring_w = (ring_w + 1) % RING_NSAMP;
  }
  float rms = n ? sqrtf((float)(e / (double)n)) : 0.0f;

  // Adaptive floor (EMA)
  float alpha = (rms < vad_thresh ? FLOOR_ALPHA_IDLE : FLOOR_ALPHA_SPK);
  noise_floor = (1.0f - alpha) * noise_floor + alpha * rms;
  vad_thresh  = max(400.0f, noise_floor * TH_MULT);

  // Draw debug every ~120 ms
  static uint32_t tLast=0; uint32_t now=millis();
  static bool vad=false;                // VAD state
  static uint32_t t_on=0, t_last_act=0;

  if (now - tLast > 120) { tLast = now; dbg_draw(rms, noise_floor, vad_thresh, useRight, vad); }

  // VAD state machine
  bool above = (rms > vad_thresh);
  if (!vad) {
    if (above) { vad = true; t_on = now; t_last_act = now; }
  } else {
    if (above) t_last_act = now;
    bool long_enough = (now - t_on) >= VAD_MIN_MS;
    bool hang_over   = (now - t_last_act) >= VAD_HANG_MS;
    if (long_enough && hang_over) {
      // extract segment (<=1200 ms)
      uint32_t dur_ms = now - t_on; if (dur_ms > 1200) dur_ms = 1200;
      int nsamp = (int)((dur_ms * SR_HZ) / 1000);
      if (nsamp < FRAME_SAMPLES*3) nsamp = FRAME_SAMPLES*3;
      static int16_t seg[SR_HZ*2];
      int preroll = (int)((VAD_PREROLL_MS * SR_HZ) / 1000);
      int start = (int)ring_w - nsamp - preroll;
      while (start < 0) start += RING_NSAMP;
      for (int i=0;i<nsamp;++i){ seg[i]=pcm_ring[(start+i)%RING_NSAMP]; }
      match_and_display(seg, nsamp);
      vad = false;
    } else if (!long_enough && hang_over) {
      vad = false;
    }
  }

  delay(1);
}