#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <driver/i2s.h>

#define USE_SPEAKER 0 // for playing SFX
#if USE_SPEAKER
  #include <Arduino.h>
  #include <esp32-hal-ledc.h>
  const int AUDIO_PIN = A0;   // A0 JST SIG → STEMMA white wire
#endif

// ===================== Pins / Qualia =====================
#define I2S_PORT      I2S_NUM_0
#define PIN_BCLK      SCK     // Qualia SCK header pin
#define PIN_WS        MOSI      // LRCLK
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
#define HOP_MS         20
#define WIN_MS         20
#define FRAME_SAMPLES  (SR_HZ * WIN_MS / 1000)
#define HOP_SAMPLES    (SR_HZ * HOP_MS / 1000)

#define KWS_NBINS      24      // must match header
#define VAD_MIN_MS     250
#define VAD_HANG_MS    180

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
KwsBank g_bank[43];

// Token names for the few you generated so far
static const char* TOKEN_NAME[43] = {
  "", "", "", "", "stop", "", "", "", "", "",
  "one", "two", "three", "four",
  "", "", "", "", "", "",
  "", "", "", "", "", "",
  "", "", "", "", "", "",
  "", "", "", "", "", "",
  "", "", "", "", ""
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
  if ((w32 != 0) && (w32 != -1) ) {
    Serial.println(s18);
  }
  
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

// ===================== Simple DSP (24-band) =====================
static float goertzel_coeff[KWS_NBINS], goertzel_sine[KWS_NBINS], goertzel_cos[KWS_NBINS];

static void init_filterbank() {
  for (int b = 0; b < KWS_NBINS; ++b) {
    float f = 200.0f + (3800.0f - 200.0f) * (float)b / (float)(KWS_NBINS - 1);
    float k = 0.5f + (FRAME_SAMPLES * f) / SR_HZ;
    float w = (2.0f * PI * k) / FRAME_SAMPLES;
    goertzel_coeff[b] = 2.0f * cosf(w);
    goertzel_sine[b]  = sinf(w);
    goertzel_cos[b]   = cosf(w);
  }
}

static void hamming(float *x, int n) {
  for (int i = 0; i < n; ++i) x[i] *= (0.54f - 0.46f * cosf(2.0f * PI * i / (n - 1)));
}

static void frame_feats_24(const int16_t* in, int ofs, float out[KWS_NBINS]) {
  static float wf[FRAME_SAMPLES];
  float mean = 0.f;
  for (int i = 0; i < FRAME_SAMPLES; ++i) mean += in[ofs + i];
  mean /= FRAME_SAMPLES;
  for (int i = 0; i < FRAME_SAMPLES; ++i) wf[i] = (float)in[ofs + i] - mean;
  hamming(wf, FRAME_SAMPLES);
  for (int b = 0; b < KWS_NBINS; ++b) {
    float s0=0, s1=0, s2=0, c=goertzel_coeff[b];
    for (int i=0;i<FRAME_SAMPLES;++i){ s0 = wf[i] + c*s1 - s2; s2 = s1; s1 = s0; }
    float real = s1 - s2 * goertzel_cos[b];
    float imag = s2 * goertzel_sine[b];
    float mag2 = real*real + imag*imag + 1e-6f;
    out[b] = logf(mag2);
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

static void resample_frames(const float* in, int in_T, float* out, int out_T) {
  if (in_T == out_T) { memcpy(out, in, sizeof(float)*in_T*KWS_NBINS); return; }
  for (int j=0;j<out_T;++j){
    float pos = (float)j * (float)(in_T - 1) / (float)(out_T - 1);
    int i0=(int)floorf(pos), i1=min(i0+1, in_T-1); float a=pos-i0;
    for (int b=0;b<KWS_NBINS;++b){
      float v0=in[i0*KWS_NBINS+b], v1=in[i1*KWS_NBINS+b];
      out[j*KWS_NBINS+b]=v0 + a*(v1-v0);
    }
  }
}

static float cos_sim_24(const float* a, const float* b) {
  float dot=0, na=0, nb=0;
  for (int i=0;i<KWS_NBINS;++i){ dot+=a[i]*b[i]; na+=a[i]*a[i]; nb+=b[i]*b[i]; }
  return dot / (sqrtf(na)*sqrtf(nb) + 1e-9f);
}

static float seq_similarity(const float* A, int TA, const float* B, int TB) {
  int T = max(TA, TB); if (T>64) T=64;
  static float Ar[64*KWS_NBINS], Br[64*KWS_NBINS];
  resample_frames(A, TA, Ar, T);
  resample_frames(B, TB, Br, T);
  float sum=0; for (int t=0;t<T;++t) sum += cos_sim_24(&Ar[t*KWS_NBINS], &Br[t*KWS_NBINS]);
  return sum / T;
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
  gfx->printf("conf=%.2f", sim);
}

// ===================== Matching helper =====================
static void match_and_display(const int16_t* segment, int nsamp) {
  const int T_max = 64;
  static float feats[T_max * KWS_NBINS];
  int T = 0;
  for (int ofs = 0; ofs + FRAME_SAMPLES <= nsamp && T < T_max; ofs += HOP_SAMPLES) {
    frame_feats_24(segment, ofs, &feats[T*KWS_NBINS]);
    ++T;
  }
  if (T < 3) return;
  znorm_frames(feats, T);

  float bestSim = -1.0f; int bestTid = -1;
  for (int tid = 0; tid < 43; ++tid) {
    KwsBank &bk = g_bank[tid];
    for (int j = 0; j < bk.n; ++j) {
      float sim = seq_similarity(feats, T, bk.tpl[j].feats, bk.tpl[j].T);
      if (sim > bestSim) { bestSim = sim; bestTid = tid; }
    }
  }
  const float TH = 0.70f;   // a bit looser while you have 1 template/word
  if (bestTid >= 0 && bestSim >= TH) {
    show_detect(TOKEN_NAME[bestTid], bestSim);
    Serial.printf("[KWS] %s  sim=%.3f\n",
                  TOKEN_NAME[bestTid] ? TOKEN_NAME[bestTid] : "(unknown)", bestSim);
  } else {
    show_detect("(none)", bestSim);
    Serial.printf("[KWS] No reliable match. best=%d(%s) sim=%.3f\n",
      bestTid, (bestTid>=0 && TOKEN_NAME[bestTid])?TOKEN_NAME[bestTid]:"-", bestSim);
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

  #if USE_SPEAKER
    if(!ledcAttach(AUDIO_PIN, 20000, 8)) {
      Serial.println("Failed to setup LEDC for audio output!");
    }

    ledcWrite(AUDIO_PIN, 128); // start silent
  #endif

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
    pcm_ring[ring_w] = (int16_t)s16[i];
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
      int start = (int)ring_w - nsamp; while (start < 0) start += RING_NSAMP;
      for (int i=0;i<nsamp;++i){ seg[i]=pcm_ring[(start+i)%RING_NSAMP]; }
      match_and_display(seg, nsamp);
      vad = false;
    } else if (!long_enough && hang_over) {
      vad = false;
    }
  }

  #if USE_SPEAKER
    static uint32_t lastBeep = 0;
    uint32_t nowbeep = millis();

    if (now - lastBeep > 2000) {
      lastBeep = now;

      // 440 Hz tone for 200 ms
      ledcWriteTone(AUDIO_PIN, 440);
      delay(200);
      ledcWriteTone(AUDIO_PIN, 0);   // 0 = stop tone (duty 0)
    }
  #endif

  delay(1);
}
