
// Sound Effect Test (Qualia ESP32-S3)
//
// Plays embedded MP3 sound effects from device_code/sounds/*.mp3.
//
// Dependencies (Arduino Library Manager):
//   - "ESP8266Audio" (works on ESP32 as well)
//
// Notes:
// - The Adafruit STEMMA Speaker PID 3885 is an analog/class-D amp, not an I2S
//   decoder. Use AudioOutputI2SNoDAC so the ESP32-S3 outputs a one-pin
//   delta-sigma waveform on A0.
// - This sketch is intentionally standalone (no mic/display) for audio testing.

#include <Arduino.h>

#include "sounds/sfx_mp3_data.h"

// ESP8266Audio (ESP32-compatible) headers
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2SNoDAC.h>

// ---------------- STEMMA Speaker Pinout ----------------
// Qualia A0 JST SIG -> STEMMA Speaker white wire.
static const int AUDIO_PIN = A0;

// --------------- Player State ----------------
static AudioGeneratorMP3* g_mp3 = nullptr;
static AudioFileSourcePROGMEM* g_src = nullptr;
static AudioOutputI2SNoDAC* g_out = nullptr;

static int g_current = 0;

static void stop_playback() {
	if (g_mp3) {
		if (g_mp3->isRunning()) g_mp3->stop();
		delete g_mp3;
		g_mp3 = nullptr;
	}
	if (g_src) {
		delete g_src;
		g_src = nullptr;
	}
}

static void play_index(int idx) {
	if (SFX_MP3_COUNT == 0) return;
	if (idx < 0) idx = (int)SFX_MP3_COUNT - 1;
	if (idx >= (int)SFX_MP3_COUNT) idx = 0;
	g_current = idx;

	stop_playback();
	g_mp3 = new AudioGeneratorMP3();
	g_src = new AudioFileSourcePROGMEM(SFX_MP3_LIST[g_current].data, SFX_MP3_LIST[g_current].len);

	Serial.printf("[SFX] Playing %d/%d: %s (%u bytes)\n",
								g_current + 1, (int)SFX_MP3_COUNT, SFX_MP3_LIST[g_current].name,
								(unsigned)SFX_MP3_LIST[g_current].len);

	g_mp3->begin(g_src, g_out);
}

static void print_menu() {
	Serial.println();
	Serial.println("=== Sound Effect Test (NoDAC + embedded MP3) ===");
	Serial.printf("STEMMA speaker signal pin: A0/GPIO %d\n", (int)AUDIO_PIN);
	Serial.println("Commands:");
	Serial.println("  n        -> next sound");
	Serial.println("  p        -> previous sound");
	Serial.println("  s        -> stop");
	Serial.println("  0..9     -> play index (0-based)");
	Serial.println("  ?        -> print menu");
	Serial.println();
	Serial.println("Available sounds:");
	for (size_t i = 0; i < SFX_MP3_COUNT; ++i) {
		Serial.printf("  %u: %s\n", (unsigned)i, SFX_MP3_LIST[i].name);
	}
	Serial.println();
}

void setup() {
	Serial.begin(115200);
	delay(200);

	if (SFX_MP3_COUNT == 0) {
		Serial.println("[SFX] No embedded MP3s found.");
		while (true) delay(1000);
	}

	g_out = new AudioOutputI2SNoDAC(AUDIO_PIN);
	g_out->SetBuffers(8, 2048);
	g_out->SetOversampling(64);
	g_out->SetGain(0.10f); // 0.0 .. 1.0

	print_menu();
	play_index(0);
}

void loop() {
	// Keep decoder running
	if (g_mp3 && g_mp3->isRunning()) {
		if (!g_mp3->loop()) {
			g_mp3->stop();
			Serial.println("[SFX] Done.");
		}
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
