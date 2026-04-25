
// Sound Effect Test (Qualia ESP32-S3)
//
// Plays sound effects from device_code/sounds/*.mp3 after converting them to
// embedded PCM with device_code/sounds/generate_sfx_pcm_header.js.
//
// Dependencies (Arduino Library Manager):
//   - "ESP8266Audio" (works on ESP32 as well)
//
// Notes:
// - The Adafruit STEMMA Speaker PID 3885 is an analog/class-D amp, not an I2S
//   decoder. This test uses normal high-frequency PWM on A0, matching the
//   electrical path that speaker_test.ino already proves works.
// - This sketch is intentionally standalone (no mic/display) for audio testing.

#include <Arduino.h>
#include <esp32-hal-ledc.h>
#include <esp_timer.h>

#include "sounds/sfx_pcm_data.h"

// ---------------- STEMMA Speaker Pinout ----------------
// Qualia A0 JST SIG -> STEMMA Speaker white wire.
static const int AUDIO_PIN = A0;
static const uint8_t PWM_RESOLUTION_BITS = 9;
static const uint16_t PCM_SILENCE = 256;
static const uint32_t PWM_CARRIERS[] = {62500, 78125, 39062, 125000};
static const size_t PWM_CARRIER_COUNT = sizeof(PWM_CARRIERS) / sizeof(PWM_CARRIERS[0]);
static const size_t FADE_SAMPLES = 384; // 24 ms at 16 kHz
static const uint32_t PRE_ROLL_MS = 30;
static const uint32_t POST_ROLL_MS = 60;

// --------------- Player State ----------------
static esp_timer_handle_t g_sample_timer = nullptr;
static const SfxPcm* g_clip = nullptr;
static volatile size_t g_sample_idx = 0;
static volatile bool g_playing = false;
static volatile uint8_t g_volume = 180; // 0..255
static int g_current = 0;
static size_t g_pwm_carrier_idx = 0;
static bool g_pwm_attached = false;

static uint16_t scale_sample(uint16_t raw) {
	int centered = (int)raw - (int)PCM_SILENCE;
	int scaled = (int)PCM_SILENCE + ((centered * (int)g_volume) / 255);
	if (scaled < 0) return 0;
	if (scaled > 511) return 511;
	return (uint16_t)scaled;
}

static uint16_t apply_envelope(uint16_t sample, size_t idx, size_t len) {
	if (FADE_SAMPLES == 0 || len == 0) return sample;

	size_t fade = FADE_SAMPLES;
	if (fade > len / 2) fade = len / 2;
	if (fade == 0) return sample;

	uint32_t gain = 65535;
	if (idx < fade) {
		gain = (uint32_t)(idx + 1) * 65535UL / (uint32_t)fade;
	} else if (idx >= len - fade) {
		gain = (uint32_t)(len - idx) * 65535UL / (uint32_t)fade;
	}

	int centered = (int)sample - (int)PCM_SILENCE;
	int shaped = (int)PCM_SILENCE + (int)((centered * (int32_t)gain) / 65535L);
	if (shaped < 0) return 0;
	if (shaped > 511) return 511;
	return (uint16_t)shaped;
}

static void sample_tick(void*) {
	const SfxPcm* clip = g_clip;
	size_t idx = g_sample_idx;
	if (!g_playing || !clip || idx >= clip->len) {
		g_playing = false;
		ledcWrite(AUDIO_PIN, PCM_SILENCE);
		return;
	}

	uint16_t raw = pgm_read_word(clip->data + idx);
	ledcWrite(AUDIO_PIN, apply_envelope(scale_sample(raw), idx, clip->len));
	g_sample_idx = idx + 1;
}

static void attach_pwm() {
	if (g_pwm_attached) return;
	if (!ledcAttach(AUDIO_PIN, PWM_CARRIERS[g_pwm_carrier_idx], PWM_RESOLUTION_BITS)) {
		Serial.println("[SFX] LEDC attach failed.");
		while (true) delay(1000);
	}
	g_pwm_attached = true;
	ledcWrite(AUDIO_PIN, PCM_SILENCE);
}

static void silence_output() {
	if (g_pwm_attached) {
		ledcWrite(AUDIO_PIN, PCM_SILENCE);
		delay(POST_ROLL_MS);
		ledcDetach(AUDIO_PIN);
		g_pwm_attached = false;
	}
	pinMode(AUDIO_PIN, INPUT);
}

static void stop_playback() {
	g_playing = false;
	if (g_sample_timer) {
		esp_timer_stop(g_sample_timer);
	}
	silence_output();
	g_clip = nullptr;
	g_sample_idx = 0;
}

static void set_pwm_carrier(size_t idx) {
	if (idx >= PWM_CARRIER_COUNT) idx = 0;
	g_pwm_carrier_idx = idx;
	if (g_pwm_attached) {
		uint32_t actual = ledcChangeFrequency(AUDIO_PIN, PWM_CARRIERS[g_pwm_carrier_idx], PWM_RESOLUTION_BITS);
		ledcWrite(AUDIO_PIN, PCM_SILENCE);
		Serial.printf("[SFX] PWM carrier requested=%u Hz actual=%u Hz\n",
									(unsigned)PWM_CARRIERS[g_pwm_carrier_idx], (unsigned)actual);
		return;
	}
	Serial.printf("[SFX] PWM carrier requested=%u Hz actual=%u Hz\n",
								(unsigned)PWM_CARRIERS[g_pwm_carrier_idx], 0U);
}

static void play_index(int idx) {
	if (SFX_PCM_COUNT == 0) return;
	if (idx < 0) idx = (int)SFX_PCM_COUNT - 1;
	if (idx >= (int)SFX_PCM_COUNT) idx = 0;
	g_current = idx;

	stop_playback();
	attach_pwm();
	delay(PRE_ROLL_MS);
	g_clip = &SFX_PCM_LIST[g_current];
	g_sample_idx = 0;
	g_playing = true;

	Serial.printf("[SFX] Playing %d/%d: %s (%u samples @ %u Hz)\n",
								g_current + 1, (int)SFX_PCM_COUNT, g_clip->name,
								(unsigned)g_clip->len, (unsigned)g_clip->sample_rate);

	uint64_t period_us = 1000000ULL / (uint64_t)g_clip->sample_rate;
	esp_timer_start_periodic(g_sample_timer, period_us);
}

static void print_menu() {
	Serial.println();
	Serial.println("=== Sound Effect Test (PWM PCM on STEMMA speaker) ===");
	Serial.printf("STEMMA speaker signal pin: A0/GPIO %d\n", (int)AUDIO_PIN);
	Serial.printf("PWM carrier: %u Hz, PCM: 16000 Hz mono unsigned 9-bit\n",
								(unsigned)PWM_CARRIERS[g_pwm_carrier_idx]);
	Serial.println("Commands:");
	Serial.println("  n        -> next sound");
	Serial.println("  p        -> previous sound");
	Serial.println("  s        -> stop");
	Serial.println("  + / -    -> volume up/down");
	Serial.println("  f        -> cycle PWM carrier");
	Serial.println("  0..9     -> play index (0-based)");
	Serial.println("  ?        -> print menu");
	Serial.println();
	Serial.println("Available sounds:");
	for (size_t i = 0; i < SFX_PCM_COUNT; ++i) {
		Serial.printf("  %u: %s\n", (unsigned)i, SFX_PCM_LIST[i].name);
	}
	Serial.println();
}

void setup() {
	Serial.begin(115200);
	delay(200);

	if (SFX_PCM_COUNT == 0) {
		Serial.println("[SFX] No embedded PCM clips found.");
		while (true) delay(1000);
	}

	Serial.printf("[SFX] Serial baud: 115200\n");
	Serial.printf("[SFX] Initial PWM carrier: %u Hz\n", (unsigned)PWM_CARRIERS[g_pwm_carrier_idx]);
	silence_output();

	const esp_timer_create_args_t timer_args = {
		.callback = &sample_tick,
		.arg = nullptr,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "sfx_pwm",
		.skip_unhandled_events = true,
	};
	if (esp_timer_create(&timer_args, &g_sample_timer) != ESP_OK) {
		Serial.println("[SFX] Sample timer create failed.");
		while (true) delay(1000);
	}

	print_menu();
	play_index(0);
}

void loop() {
	if (g_clip && !g_playing) {
		esp_timer_stop(g_sample_timer);
		silence_output();
		g_clip = nullptr;
		Serial.println("[SFX] Done.");
	}

	// Serial control
	while (Serial.available()) {
		int c = Serial.read();
		if (c < 0) break;
		if (c == '\n' || c == '\r') continue;

		if (c == 'n') {
			play_index(g_current + 1);
		} else if (c == 'p') {
			play_index(g_current - 1);
		} else if (c == 's') {
			stop_playback();
			Serial.println("[SFX] Stopped.");
		} else if (c == '+') {
			g_volume = (g_volume > 230) ? 255 : (uint8_t)(g_volume + 25);
			Serial.printf("[SFX] Volume=%u/255\n", (unsigned)g_volume);
		} else if (c == '-') {
			g_volume = (g_volume < 25) ? 0 : (uint8_t)(g_volume - 25);
			Serial.printf("[SFX] Volume=%u/255\n", (unsigned)g_volume);
		} else if (c == 'f') {
			bool was_playing = g_playing;
			int replay_idx = g_current;
			stop_playback();
			set_pwm_carrier(g_pwm_carrier_idx + 1);
			if (was_playing) play_index(replay_idx);
		} else if (c == '?') {
			print_menu();
		} else if (c >= '0' && c <= '9') {
			int idx = c - '0';
			play_index(idx);
		} else {
			Serial.printf("[SFX] Unknown key '%c'\n", (char)c);
		}
	}

	delay(1);
}
