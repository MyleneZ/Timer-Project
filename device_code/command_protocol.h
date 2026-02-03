#pragma once

#include <stdint.h>

// Command tokens shared between BLE and demo mode.
// Kept in a header so Arduino's auto-prototype generation can see these types.

enum VoiceCommand : uint8_t {
  CMD_NONE = 0,
  CMD_SET = 1,
  CMD_CANCEL = 2,
  CMD_ADD = 3,
  CMD_MINUS = 4,
  CMD_STOP = 5
};

struct ParsedCommand {
  VoiceCommand cmd;
  char name[16];
  uint32_t duration;
};
