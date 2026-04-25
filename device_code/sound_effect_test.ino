
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

// --------------- Player State ----------------
static esp_timer_handle_t g_sample_timer = nullptr;
static const SfxPcm* g_clip = nullptr;
static volatile size_t g_sample_idx = 0;
static volatile bool g_playing = false;
static volatile uint8_t g_volume = 180; // 0..255
static int g_current = 0;

static uint8_t scale_sample(uint8_t raw) {
	int centered = (int)raw - 128;
	int scaled = 128 + ((centered * (int)g_volume) / 255);
	if (scaled < 0) return 0;
	if (scaled > 255) return 255;
	return (uint8_t)scaled;
}

static void sample_tick(void*) {
	const SfxPcm* clip = g_clip;
	size_t idx = g_sample_idx;
	if (!g_playing || !clip || idx >= clip->len) {
		g_playing = false;
		ledcWrite(AUDIO_PIN, 0);
		return;
	}

	uint8_t raw = pgm_read_byte(clip->data + idx);
	ledcWrite(AUDIO_PIN, scale_sample(raw));
	g_sample_idx = idx + 1;
}

static void stop_playback() {
	g_playing = false;
	if (g_sample_timer) {
		esp_timer_stop(g_sample_timer);
	}
	ledcWrite(AUDIO_PIN, 0);
	g_clip = nullptr;
	g_sample_idx = 0;
}

static void play_index(int idx) {
	if (SFX_PCM_COUNT == 0) return;
	if (idx < 0) idx = (int)SFX_PCM_COUNT - 1;
	if (idx >= (int)SFX_PCM_COUNT) idx = 0;
	g_current = idx;

	stop_playback();
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
	Serial.println("PWM carrier: 62500 Hz, PCM: 16000 Hz mono unsigned 8-bit");
	Serial.println("Commands:");
	Serial.println("  n        -> next sound");
	Serial.println("  p        -> previous sound");
	Serial.println("  s        -> stop");
	Serial.println("  + / -    -> volume up/down");
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

	if (!ledcAttach(AUDIO_PIN, 62500, 8)) {
		Serial.println("[SFX] LEDC attach failed.");
		while (true) delay(1000);
	}
	ledcWrite(AUDIO_PIN, 0);

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
		ledcWrite(AUDIO_PIN, 0);
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
