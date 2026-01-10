
// Sound Effect Test (Qualia ESP32-S3)
//
// Plays embedded MP3 sound effects from device_code/sounds/*.mp3 via I2S.
//
// Dependencies (Arduino Library Manager):
//   - "ESP8266Audio" (works on ESP32 as well)
//
// Notes:
// - You MUST set the I2S speaker pinout below to match your hardware.
// - This sketch is intentionally standalone (no mic) so I2S is used only for TX.

#include <Arduino.h>

#include "sounds/sfx_mp3_data.h"

// ESP8266Audio (ESP32-compatible) headers
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

// ---------------- I2S Speaker Pinout ----------------
// Set these pins to the pins connected to your I2S speaker amp (e.g., MAX98357A).
// If you're using a different output method (PWM on A0), this sketch won't apply.

#ifndef I2S_SPK_BCLK
#define I2S_SPK_BCLK SCK
#endif

#ifndef I2S_SPK_LRC
#define I2S_SPK_LRC  MOSI
#endif

#ifndef I2S_SPK_DOUT
#define I2S_SPK_DOUT A0
#endif

// --------------- Player State ----------------
static AudioGeneratorMP3* g_mp3 = nullptr;
static AudioFileSourcePROGMEM* g_src = nullptr;
static AudioOutputI2S* g_out = nullptr;

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
	Serial.println("=== Sound Effect Test (I2S + embedded MP3) ===");
	Serial.printf("I2S pinout: BCLK=%d  LRC=%d  DOUT=%d\n", (int)I2S_SPK_BCLK, (int)I2S_SPK_LRC, (int)I2S_SPK_DOUT);
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

	g_out = new AudioOutputI2S();
	g_out->SetPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DOUT);
	g_out->SetGain(0.6f); // 0.0 .. 1.0

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

