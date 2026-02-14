/*
 * Timer Device - Nicla Voice Production Firmware (FIXED)
 *
 * Role split:
 *  - Nicla Voice: BLE Central (client) + on-device KWS via NDP120
 *  - Qualia ESP32-S3: BLE Peripheral (server) advertising Nordic UART Service (NUS)
 *
 * This sketch:
 *  1) Loads your NDP120 speech model from external flash
 *  2) Receives recognition events as string labels via NDP.onEvent(cb)
 *  3) Converts labels -> token indices
 *  4) Parses tokens into a TimerCommand
 *  5) Sends TimerCommand to Qualia by writing to NUS RX characteristic (6E400002...)
 */

#include <Arduino.h>
#include <NDP.h>
#include <ArduinoBLE.h>
#include <Nicla_System.h>   // for nicla::leds + color constants

// ======================= CONFIG =======================
#define DEBUG_SERIAL 1
#define LED_FEEDBACK 1

#define BLE_SCAN_RESTART_MS 2500
#define BLE_CONNECT_TIMEOUT_MS 20000
#define COMMAND_TIMEOUT_MS 1500
#define MAX_TOKENS 10

// ======================= NUS UUIDs (MUST MATCH QUALIA) =======================
#define SERVICE_UUID             "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E" // Qualia RX (we WRITE here)
#define CHARACTERISTIC_UUID_TX   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E" // Qualia TX (optional notify)

static const char* TARGET_DEVICE_NAME = "TimerDevice"; // Qualia advertises NimBLEDevice::init("TimerDevice")

// ======================= TOKEN IDS (must match your model/templates) =======================
enum TokenId : uint8_t {
  TOK_SET = 0,
  TOK_CANCEL = 1,
  TOK_ADD = 2,
  TOK_MINUS = 3,
  TOK_STOP = 4,
  TOK_TIMER = 5,
  TOK_MINUTE = 6,
  TOK_MINUTES = 7,
  TOK_HOUR = 8,
  TOK_HOURS = 9,
  // Numbers 1-19
  TOK_ONE = 10,
  TOK_TWO = 11,
  TOK_THREE = 12,
  TOK_FOUR = 13,
  TOK_FIVE = 14,
  TOK_SIX = 15,
  TOK_SEVEN = 16,
  TOK_EIGHT = 17,
  TOK_NINE = 18,
  TOK_TEN = 19,
  TOK_ELEVEN = 20,
  TOK_TWELVE = 21,
  TOK_THIRTEEN = 22,
  TOK_FOURTEEN = 23,
  TOK_FIFTEEN = 24,
  TOK_SIXTEEN = 25,
  TOK_SEVENTEEN = 26,
  TOK_EIGHTEEN = 27,
  TOK_NINETEEN = 28,
  // Tens
  TOK_TWENTY = 29,
  TOK_THIRTY = 30,
  TOK_FORTY = 31,
  TOK_FIFTY = 32,
  TOK_SIXTY = 33,
  TOK_SEVENTY = 34,
  TOK_EIGHTY = 35,
  TOK_NINETY = 36,
  // Timer names
  TOK_BAKING = 37,
  TOK_COOKING = 38,
  TOK_BREAK = 39,
  TOK_HOMEWORK = 40,
  TOK_EXERCISE = 41,
  TOK_WORKOUT = 42,
  TOK_UNKNOWN = 255
};

// Token name lookup (what your NDP model should output as labels)
static const char* TOKEN_NAMES[] = {
  "set", "cancel", "add", "minus", "stop", "timer",
  "minute", "minutes", "hour", "hours",
  "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
  "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
  "sixteen", "seventeen", "eighteen", "nineteen",
  "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
  "baking", "cooking", "break", "homework", "exercise", "workout"
};
static const int TOKEN_COUNT = (int)(sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0]));

// Timer name strings (must match what Qualia expects)
static const char* TIMER_NAMES[] = { "Baking", "Cooking", "Break", "Homework", "Exercise", "Workout" };
static const int TIMER_NAME_COUNT = 6;

// ======================= PARSE STATE =======================
struct ParseState {
  uint8_t tokens[MAX_TOKENS];
  int count;
  uint32_t lastTokenTime;
  bool commandComplete;
};

static ParseState parseState = { {0}, 0, 0, false };

// ======================= COMMAND OUTPUT =======================
enum CommandType {
  CMD_NONE = 0,
  CMD_SET,
  CMD_CANCEL,
  CMD_ADD,
  CMD_MINUS,
  CMD_STOP
};

struct TimerCommand {
  CommandType type;
  char name[16];
  uint32_t durationSeconds;
};

// ======================= BLE STATE (CENTRAL) =======================
static bool bleConnected = false;
static BLEDevice connectedPeripheral;
static BLECharacteristic remoteRxChar; // write to Qualia RX (6E400002...)
static BLECharacteristic remoteTxChar; // optional notify

static uint32_t lastScanRestart = 0;

// ======================= LED FEEDBACK =======================
#if LED_FEEDBACK
static void ledFlash(const int colorConst, int ms = 120) {
  nicla::leds.begin();
  nicla::leds.setColor(colorConst);
  delay(ms);
  nicla::leds.setColor(off);
  nicla::leds.end();
}
#endif

// ======================= STRING UTIL =======================
static void strToLower(char* s) {
  for (; *s; ++s) {
    if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
  }
}

static void trimSpaces(char* s) {
  // leading
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  // copy to start
  char* p = s;
  // find end
  char* end = p + strlen(p);
  while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
  *end = '\0';
}

static void extractLabelToken(const char* in, char* out, size_t outSz) {
  // Many NDP labels look like "NNO:alexa" or "NNO:set"
  // We take everything after the last ':' if present.
  const char* lastColon = strrchr(in, ':');
  const char* start = lastColon ? lastColon + 1 : in;

  strncpy(out, start, outSz - 1);
  out[outSz - 1] = '\0';

  // normalize
  strToLower(out);
  // trim (in-place-ish)
  // (simple trim for end)
  size_t n = strlen(out);
  while (n && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t')) {
    out[n - 1] = '\0';
    n--;
  }
  // trim front by shifting if needed
  size_t i = 0;
  while (out[i] == ' ' || out[i] == '\t') i++;
  if (i) memmove(out, out + i, strlen(out + i) + 1);
}

// Map recognized label -> token index
static int tokenIndexFromLabel(const char* label) {
  char norm[32];
  extractLabelToken(label, norm, sizeof(norm));

  for (int i = 0; i < TOKEN_COUNT; i++) {
    if (strcmp(norm, TOKEN_NAMES[i]) == 0) return i;
  }
  return -1;
}

// ======================= NUMBER PARSING =======================
static int parseNumber(const uint8_t* tokens, int start, int count) {
  if (start >= count) return -1;

  int result = 0;
  int i = start;

  if (tokens[i] >= TOK_TWENTY && tokens[i] <= TOK_NINETY) {
    int tens = (tokens[i] - TOK_TWENTY + 2) * 10;  // 20, 30, 40...
    result = tens;
    i++;

    if (i < count && tokens[i] >= TOK_ONE && tokens[i] <= TOK_NINE) {
      result += (tokens[i] - TOK_ONE + 1);
      i++;
    }
  } else if (tokens[i] >= TOK_ONE && tokens[i] <= TOK_NINETEEN) {
    result = tokens[i] - TOK_ONE + 1;
    i++;
  }

  return (result > 0) ? result : -1;
}

// ======================= TIMER NAME PARSING =======================
static const char* parseTimerName(const uint8_t* tokens, int start, int count) {
  if (start >= count) return "Timer";
  uint8_t tok = tokens[start];
  if (tok >= TOK_BAKING && tok <= TOK_WORKOUT) {
    int idx = (int)(tok - TOK_BAKING);
    if (idx >= 0 && idx < TIMER_NAME_COUNT) return TIMER_NAMES[idx];
  }
  return "Timer";
}

// ======================= COMMAND PARSING =======================
static TimerCommand parseCommand(const ParseState& state) {
  TimerCommand cmd;
  cmd.type = CMD_NONE;
  cmd.name[0] = '\0';
  cmd.durationSeconds = 0;

  if (state.count == 0) return cmd;

  const uint8_t firstToken = state.tokens[0];

  if (firstToken == TOK_STOP) {
    cmd.type = CMD_STOP;
    return cmd;
  }

  if (firstToken == TOK_SET) {
    cmd.type = CMD_SET;
    int idx = 1;

    const char* name = parseTimerName(state.tokens, idx, state.count);
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';

    // If name token present, consume it
    if (idx < state.count && state.tokens[idx] >= TOK_BAKING && state.tokens[idx] <= TOK_WORKOUT) idx++;

    // Optional "timer"
    if (idx < state.count && state.tokens[idx] == TOK_TIMER) idx++;

    // Number
    int number = parseNumber(state.tokens, idx, state.count);
    if (number < 0) number = 5;

    // Consume number tokens (tens + optional ones OR 1..19)
    if (idx < state.count) {
      if (state.tokens[idx] >= TOK_TWENTY && state.tokens[idx] <= TOK_NINETY) {
        idx++;
        if (idx < state.count && state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINE) idx++;
      } else if (state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINETEEN) {
        idx++;
      }
    }

    // Units
    bool isHours = false;
    if (idx < state.count && (state.tokens[idx] == TOK_HOUR || state.tokens[idx] == TOK_HOURS)) {
      isHours = true;
    }
    cmd.durationSeconds = (uint32_t)number * (isHours ? 3600UL : 60UL);
    return cmd;
  }

  if (firstToken == TOK_CANCEL) {
    cmd.type = CMD_CANCEL;
    const char* name = parseTimerName(state.tokens, 1, state.count);
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';
    return cmd;
  }

  if (firstToken == TOK_ADD || firstToken == TOK_MINUS) {
    cmd.type = (firstToken == TOK_ADD) ? CMD_ADD : CMD_MINUS;

    int idx = 1;
    int number = parseNumber(state.tokens, idx, state.count);
    if (number < 0) number = 1;

    // Consume number tokens properly
    if (idx < state.count) {
      if (state.tokens[idx] >= TOK_TWENTY && state.tokens[idx] <= TOK_NINETY) {
        idx++;
        if (idx < state.count && state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINE) idx++;
      } else if (state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINETEEN) {
        idx++;
      }
    }

    // Units
    bool isHours = false;
    if (idx < state.count) {
      if (state.tokens[idx] == TOK_HOUR || state.tokens[idx] == TOK_HOURS) {
        isHours = true;
        idx++;
      } else if (state.tokens[idx] == TOK_MINUTE || state.tokens[idx] == TOK_MINUTES) {
        idx++;
      }
    }

    // Find timer name anywhere after that (robust to “to/from” words you might add later)
    const char* name = "Timer";
    for (int i = idx; i < state.count; i++) {
      if (state.tokens[i] >= TOK_BAKING && state.tokens[i] <= TOK_WORKOUT) {
        name = TIMER_NAMES[state.tokens[i] - TOK_BAKING];
        break;
      }
    }

    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';

    cmd.durationSeconds = (uint32_t)number * (isHours ? 3600UL : 60UL);
    return cmd;
  }

  return cmd;
}

// ======================= BLE SEND =======================
static void sendCommandToQualia(const TimerCommand& cmd) {
  if (!bleConnected || !connectedPeripheral || !remoteRxChar) {
    #if DEBUG_SERIAL
    Serial.println("[BLE] Not connected or RX char missing; cannot send.");
    #endif
    return;
  }

  char buffer[64];

  switch (cmd.type) {
    case CMD_SET:
      snprintf(buffer, sizeof(buffer), "CMD:SET,NAME:%s,DURATION:%lu",
               cmd.name, (unsigned long)cmd.durationSeconds);
      break;
    case CMD_CANCEL:
      snprintf(buffer, sizeof(buffer), "CMD:CANCEL,NAME:%s", cmd.name);
      break;
    case CMD_ADD:
      snprintf(buffer, sizeof(buffer), "CMD:ADD,NAME:%s,DURATION:%lu",
               cmd.name, (unsigned long)cmd.durationSeconds);
      break;
    case CMD_MINUS:
      snprintf(buffer, sizeof(buffer), "CMD:MINUS,NAME:%s,DURATION:%lu",
               cmd.name, (unsigned long)cmd.durationSeconds);
      break;
    case CMD_STOP:
      snprintf(buffer, sizeof(buffer), "CMD:STOP");
      break;
    default:
      return;
  }

  #if DEBUG_SERIAL
  Serial.print("[BLE] -> ");
  Serial.println(buffer);
  #endif

  // Prefer Write Without Response if supported, otherwise fallback to Write.
  bool ok = false;
  if (remoteRxChar.canWriteWithoutResponse()) {
    ok = remoteRxChar.writeValue((const uint8_t*)buffer, (int)strlen(buffer), false);
  } else if (remoteRxChar.canWrite()) {
    ok = remoteRxChar.writeValue((const uint8_t*)buffer, (int)strlen(buffer));
  }

  #if LED_FEEDBACK
  if (ok) ledFlash(blue, 90);
  else    ledFlash(red, 140);
  #endif

  #if DEBUG_SERIAL
  Serial.println(ok ? "[BLE] Write OK" : "[BLE] Write FAILED");
  #endif
}

// ======================= NDP EVENT HANDLING =======================
// The NDP library calls this with a label string (like AlexaDemo).
static volatile bool g_tokenPending = false;
static volatile int  g_pendingTokenIndex = -1;

void ndpOnEvent(char* label) {
  // Convert label -> token idx
  const int idx = tokenIndexFromLabel(label);

  #if DEBUG_SERIAL
  Serial.print("[NDP] label=");
  Serial.print(label);
  Serial.print(" -> idx=");
  Serial.println(idx);
  #endif

  if (idx >= 0 && idx < TOKEN_COUNT) {
    // Mark pending token for loop() to consume
    g_pendingTokenIndex = idx;
    g_tokenPending = true;
  }

  #if LED_FEEDBACK
  // Quick “heard something” flash (green)
  ledFlash(green, 40);
  #endif
}

// ======================= TOKEN BUFFERING =======================
static void pushToken(int tokenIdx) {
  if (tokenIdx < 0 || tokenIdx >= TOKEN_COUNT) return;

  #if DEBUG_SERIAL
  Serial.print("[KWS] token ");
  Serial.print(tokenIdx);
  Serial.print(" = ");
  Serial.println(TOKEN_NAMES[tokenIdx]);
  #endif

  if (parseState.count < MAX_TOKENS) {
    parseState.tokens[parseState.count++] = (uint8_t)tokenIdx;
    parseState.lastTokenTime = millis();

    // Single word command completes immediately
    if (tokenIdx == TOK_STOP) parseState.commandComplete = true;
  } else {
    #if DEBUG_SERIAL
    Serial.println("[KWS] Parse buffer full; dropping token.");
    #endif
  }
}

static void maybeFinalizeCommand() {
  if (parseState.count == 0) return;

  const uint32_t now = millis();
  if (parseState.commandComplete || (now - parseState.lastTokenTime > COMMAND_TIMEOUT_MS)) {
    const TimerCommand cmd = parseCommand(parseState);

    #if DEBUG_SERIAL
    Serial.print("[CMD] type=");
    Serial.print((int)cmd.type);
    Serial.print(" name=");
    Serial.print(cmd.name);
    Serial.print(" dur=");
    Serial.println((unsigned long)cmd.durationSeconds);
    #endif

    if (cmd.type != CMD_NONE) {
      sendCommandToQualia(cmd);
    }

    // reset buffer
    parseState.count = 0;
    parseState.commandComplete = false;
  }
}

// ======================= BLE CENTRAL CONNECT =======================
static void startScan() {
  BLE.stopScan();

  // You can scan by UUID, but not all peripherals include service UUID in adverts.
  // Scanning generally + matching by localName is most robust.
  BLE.scan();

  lastScanRestart = millis();

  #if DEBUG_SERIAL
  Serial.println("[BLE] Scanning...");
  #endif
}

static bool connectToQualia() {
  const uint32_t start = millis();

  while (millis() - start < BLE_CONNECT_TIMEOUT_MS) {
    BLE.poll();

    BLEDevice found = BLE.available();
    if (!found) continue;

    const String name = found.localName();

    #if DEBUG_SERIAL
    Serial.print("[BLE] Found: ");
    Serial.println(name);
    #endif

    if (name != TARGET_DEVICE_NAME) continue;

    BLE.stopScan();

    #if DEBUG_SERIAL
    Serial.println("[BLE] Connecting...");
    #endif

    if (!found.connect()) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] Connect failed.");
      #endif
      startScan();
      return false;
    }

    if (!found.discoverAttributes()) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] discoverAttributes failed.");
      #endif
      found.disconnect();
      startScan();
      return false;
    }

    BLEService nus = found.service(SERVICE_UUID);
    if (!nus) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] NUS service not found.");
      #endif
      found.disconnect();
      startScan();
      return false;
    }

    BLECharacteristic rx = found.characteristic(CHARACTERISTIC_UUID_RX);
    if (!rx) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] RX characteristic not found (6E400002...).");
      #endif
      found.disconnect();
      startScan();
      return false;
    }

    // Optional notify characteristic
    BLECharacteristic tx = found.characteristic(CHARACTERISTIC_UUID_TX);

    connectedPeripheral = found;
    remoteRxChar = rx;
    remoteTxChar = tx;
    bleConnected = true;

    #if DEBUG_SERIAL
    Serial.println("[BLE] Connected and ready to write.");
    #endif

    #if LED_FEEDBACK
    ledFlash(blue, 120);
    #endif

    return true;
  }

  return false;
}

static void maintainBleConnection() {
  if (bleConnected) {
    if (!connectedPeripheral.connected()) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] Disconnected.");
      #endif
      bleConnected = false;
      startScan();
    }
    return;
  }

  // If scanning too long, restart scan periodically
  if (millis() - lastScanRestart > BLE_SCAN_RESTART_MS) {
    startScan();
  }

  // Opportunistically attempt connection when we see the device
  connectToQualia();
}

// ======================= SETUP / LOOP =======================
void setup() {
  #if DEBUG_SERIAL
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("\n[BOOT] Nicla Voice Timer Controller (fixed) starting...");
  #endif

  #if LED_FEEDBACK
  nicla::leds.begin();
  nicla::leds.setColor(red);
  delay(120);
  nicla::leds.setColor(off);
  nicla::leds.end();
  #endif

  #if DEBUG_SERIAL
  Serial.println("[NDP] begin(model)...");
  #endif

  // Ensure timer_voice_model.bin is on external flash.
  if (!NDP.begin("timer_voice_model.bin")) {
    #if DEBUG_SERIAL
    Serial.println("[NDP] ERROR: NDP.begin failed (model missing / flash not formatted / wrong filename).");
    #endif
    #if LED_FEEDBACK
    while (true) { ledFlash(red, 120); delay(400); }
    #endif
  }

  // Use AlexaDemo-style callback signature: void cb(char* label)
  NDP.onEvent(ndpOnEvent);

  #if DEBUG_SERIAL
  Serial.println("[BLE] begin()...");
  #endif

  if (!BLE.begin()) {
    #if DEBUG_SERIAL
    Serial.println("[BLE] ERROR: BLE.begin failed.");
    #endif
    #if LED_FEEDBACK
    while (true) { ledFlash(red, 120); delay(400); }
    #endif
  }

  BLE.setLocalName("NiclaVoice_Timer");
  startScan();

  #if DEBUG_SERIAL
  Serial.println("[BOOT] Ready. Say commands once connected to Qualia.");
  Serial.println("       Serial sim:  sim set | sim baking | sim five | sim minutes");
  #endif
}

void loop() {
  // Drive BLE and NDP internals
  BLE.poll();
  NDP.poll();

  maintainBleConnection();

  // Consume pending token recognized from NDP callback
  if (g_tokenPending) {
    g_tokenPending = false;
    pushToken(g_pendingTokenIndex);
  }

  // Finalize command if timeout
  maybeFinalizeCommand();

  // Serial simulator (lets you test parser + BLE without NDP)
  #if DEBUG_SERIAL
  while (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("sim ")) {
      String tokenStr = input.substring(4);
      tokenStr.trim();

      int tokenIdx = -1;

      // number?
      bool numeric = true;
      for (size_t i = 0; i < tokenStr.length(); i++) {
        if (tokenStr[i] < '0' || tokenStr[i] > '9') { numeric = false; break; }
      }
      if (numeric) tokenIdx = tokenStr.toInt();

      if (!numeric) {
        for (int i = 0; i < TOKEN_COUNT; i++) {
          if (tokenStr.equalsIgnoreCase(TOKEN_NAMES[i])) {
            tokenIdx = i;
            break;
          }
        }
      }

      if (tokenIdx >= 0 && tokenIdx < TOKEN_COUNT) {
        pushToken(tokenIdx);
      } else {
        Serial.print("Unknown token: ");
        Serial.println(tokenStr);
      }
    } else if (input == "status") {
      Serial.print("BLE connected: ");
      Serial.println(bleConnected ? "yes" : "no");
      Serial.print("Parse buffer tokens: ");
      Serial.println(parseState.count);
    } else if (input == "tokens") {
      for (int i = 0; i < TOKEN_COUNT; i++) {
        Serial.print(i);
        Serial.print(": ");
        Serial.println(TOKEN_NAMES[i]);
      }
    } else if (input == "help") {
      Serial.println("\n=== Commands ===");
      Serial.println("  sim <token>  - Simulate token detection (name or index)");
      Serial.println("  status       - Show BLE + parse buffer status");
      Serial.println("  tokens       - List all tokens");
      Serial.println("Example: sim set, sim baking, sim five, sim minutes");
    }
  }
  #endif

  delay(5);
}
