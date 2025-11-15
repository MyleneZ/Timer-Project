// Qualia ESP32-S3 + Adafruit STEMMA Speaker (A0 JST connector)

#include <Arduino.h>
#include <esp32-hal-ledc.h>

const int AUDIO_PIN = A0;   // A0 JST SIG → STEMMA white wire
const int AUDIO_CH  = 0;    // LEDC channel

void setup() {
  // Attach PWM (LEDC) to A0 at 10 kHz base frequency, 8-bit resolution
  ledcAttach(AUDIO_PIN, 10000, 8);
//   ledcSetup(AUDIO_CH, 10000, 8);  // 10 kHz, 8-bit
}

void loop() {
  // 440 Hz tone for 500 ms
  ledcWriteTone(AUDIO_CH, 440);
  delay(500);

  // Silence for 500 ms
  ledcWriteTone(AUDIO_CH, 0);
  delay(500);
}
