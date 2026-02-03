/*
 * Timer Device - Nicla Voice Production Firmware
 * 
 * Hardware:
 *   - Arduino Nicla Voice (with NDP120 for voice recognition)
 * 
 * This firmware runs on the Nicla Voice and:
 *   1. Performs on-device keyword spotting using the NDP120
 *   2. Parses voice commands into structured timer commands
 *   3. Sends commands to the Qualia ESP32-S3 via BLE
 * 
 * Voice Command Patterns:
 *   - "Set [timer_name] timer [number] minute(s)/hour(s)"
 *   - "Cancel [timer_name] timer"
 *   - "Add [number] minute(s)/hour(s) to [timer_name] timer"
 *   - "Minus [number] minute(s)/hour(s) from [timer_name] timer"
 *   - "Stop" (stops all alarms)
 */

#include <Arduino.h>
#include <NDP.h>
#include <ArduinoBLE.h>

// ======================= CONFIGURATION =======================
#define DEBUG_SERIAL      1     // Enable debug output
#define LED_FEEDBACK      1     // Enable LED feedback for voice detection
#define BLE_CONNECT_TIMEOUT_MS 30000  // 30 seconds to connect

// ======================= NDP120 MODEL =======================
// The NDP120 model should be trained with these keywords
// Model file: timer_voice_model.bin (to be loaded via NDP library)

// ======================= NORDIC UART SERVICE =======================
// UUIDs must match the Qualia's production.ino
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEService uartService(SERVICE_UUID);
BLECharacteristic txCharacteristic(CHARACTERISTIC_UUID_TX, BLEWrite | BLEWriteWithoutResponse, 64);
BLECharacteristic rxCharacteristic(CHARACTERISTIC_UUID_RX, BLENotify, 64);

bool bleConnected = false;
String targetDeviceName = "TimerDevice";

// ======================= TOKEN IDS (must match kws_templates.h) =======================
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

// Token name lookup (for debug)
static const char* TOKEN_NAMES[] = {
  "set", "cancel", "add", "minus", "stop", "timer",
  "minute", "minutes", "hour", "hours",
  "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
  "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", 
  "sixteen", "seventeen", "eighteen", "nineteen",
  "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
  "baking", "cooking", "break", "homework", "exercise", "workout"
};
const int TOKEN_COUNT = sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0]);

// Timer name strings
static const char* TIMER_NAMES[] = {
  "Baking", "Cooking", "Break", "Homework", "Exercise", "Workout"
};
static const int TIMER_NAME_COUNT = 6;

// ======================= COMMAND PARSING STATE =======================
#define MAX_TOKENS 10
struct ParseState {
  uint8_t tokens[MAX_TOKENS];
  int count;
  uint32_t lastTokenTime;
  bool commandComplete;
};

static ParseState parseState = {{0}, 0, 0, false};

// Timeout to consider a command complete (ms after last token)
#define COMMAND_TIMEOUT_MS 1500

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

// ======================= NUMBER PARSING =======================
static int parseNumber(uint8_t* tokens, int start, int count) {
  if (start >= count) return -1;
  
  int result = 0;
  int i = start;
  
  // Check for tens place (twenty, thirty, etc.)
  if (tokens[i] >= TOK_TWENTY && tokens[i] <= TOK_NINETY) {
    int tens = (tokens[i] - TOK_TWENTY + 2) * 10;  // 20, 30, 40...
    result = tens;
    i++;
    
    // Check for ones place
    if (i < count && tokens[i] >= TOK_ONE && tokens[i] <= TOK_NINE) {
      result += (tokens[i] - TOK_ONE + 1);
      i++;
    }
  }
  // Check for teens or single digits (1-19)
  else if (tokens[i] >= TOK_ONE && tokens[i] <= TOK_NINETEEN) {
    result = tokens[i] - TOK_ONE + 1;
    i++;
  }
  
  return (result > 0) ? result : -1;
}

// ======================= TIMER NAME PARSING =======================
static const char* parseTimerName(uint8_t* tokens, int start, int count) {
  if (start >= count) return "Timer";
  
  uint8_t tok = tokens[start];
  if (tok >= TOK_BAKING && tok <= TOK_WORKOUT) {
    return TIMER_NAMES[tok - TOK_BAKING];
  }
  
  return "Timer";  // Default name
}

// ======================= COMMAND PARSING =======================
static TimerCommand parseCommand(ParseState& state) {
  TimerCommand cmd = {CMD_NONE, "", 0};
  
  if (state.count == 0) return cmd;
  
  uint8_t firstToken = state.tokens[0];
  
  // "stop" - simple single-word command
  if (firstToken == TOK_STOP) {
    cmd.type = CMD_STOP;
    return cmd;
  }
  
  // "set [name] timer [number] minute(s)/hour(s)"
  if (firstToken == TOK_SET) {
    cmd.type = CMD_SET;
    int idx = 1;
    
    // Optional timer name
    const char* name = parseTimerName(state.tokens, idx, state.count);
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';
    
    // Skip timer name token if present
    if (state.tokens[idx] >= TOK_BAKING && state.tokens[idx] <= TOK_WORKOUT) {
      idx++;
    }
    
    // Skip "timer" keyword if present
    if (idx < state.count && state.tokens[idx] == TOK_TIMER) {
      idx++;
    }
    
    // Parse number
    int number = parseNumber(state.tokens, idx, state.count);
    if (number < 0) number = 5;  // Default 5 minutes
    
    // Skip number tokens
    while (idx < state.count && 
           ((state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINETY) ||
            state.tokens[idx] == TOK_TIMER)) {
      idx++;
    }
    
    // Check for minutes or hours
    bool isHours = false;
    if (idx < state.count) {
      if (state.tokens[idx] == TOK_HOUR || state.tokens[idx] == TOK_HOURS) {
        isHours = true;
      }
    }
    
    cmd.durationSeconds = number * (isHours ? 3600 : 60);
    return cmd;
  }
  
  // "cancel [name] timer"
  if (firstToken == TOK_CANCEL) {
    cmd.type = CMD_CANCEL;
    int idx = 1;
    
    const char* name = parseTimerName(state.tokens, idx, state.count);
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';
    
    return cmd;
  }
  
  // "add [number] minute(s)/hour(s) to [name] timer"
  if (firstToken == TOK_ADD) {
    cmd.type = CMD_ADD;
    int idx = 1;
    
    // Parse number first
    int number = parseNumber(state.tokens, idx, state.count);
    if (number < 0) number = 1;
    
    // Skip number tokens
    while (idx < state.count && 
           state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINETY) {
      idx++;
    }
    
    // Check for minutes or hours
    bool isHours = false;
    if (idx < state.count) {
      if (state.tokens[idx] == TOK_HOUR || state.tokens[idx] == TOK_HOURS) {
        isHours = true;
        idx++;
      } else if (state.tokens[idx] == TOK_MINUTE || state.tokens[idx] == TOK_MINUTES) {
        idx++;
      }
    }
    
    // Find timer name (may be after "to")
    const char* name = "Timer";
    for (int i = idx; i < state.count; i++) {
      if (state.tokens[i] >= TOK_BAKING && state.tokens[i] <= TOK_WORKOUT) {
        name = TIMER_NAMES[state.tokens[i] - TOK_BAKING];
        break;
      }
    }
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';
    
    cmd.durationSeconds = number * (isHours ? 3600 : 60);
    return cmd;
  }
  
  // "minus [number] minute(s)/hour(s) from [name] timer"
  if (firstToken == TOK_MINUS) {
    cmd.type = CMD_MINUS;
    int idx = 1;
    
    int number = parseNumber(state.tokens, idx, state.count);
    if (number < 0) number = 1;
    
    while (idx < state.count && 
           state.tokens[idx] >= TOK_ONE && state.tokens[idx] <= TOK_NINETY) {
      idx++;
    }
    
    bool isHours = false;
    if (idx < state.count) {
      if (state.tokens[idx] == TOK_HOUR || state.tokens[idx] == TOK_HOURS) {
        isHours = true;
        idx++;
      } else if (state.tokens[idx] == TOK_MINUTE || state.tokens[idx] == TOK_MINUTES) {
        idx++;
      }
    }
    
    const char* name = "Timer";
    for (int i = idx; i < state.count; i++) {
      if (state.tokens[i] >= TOK_BAKING && state.tokens[i] <= TOK_WORKOUT) {
        name = TIMER_NAMES[state.tokens[i] - TOK_BAKING];
        break;
      }
    }
    strncpy(cmd.name, name, 15);
    cmd.name[15] = '\0';
    
    cmd.durationSeconds = number * (isHours ? 3600 : 60);
    return cmd;
  }
  
  return cmd;
}

// ======================= BLE COMMUNICATION =======================
static void sendCommand(TimerCommand& cmd) {
  if (!bleConnected) {
    #if DEBUG_SERIAL
    Serial.println("[BLE] Not connected, cannot send command");
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
  Serial.printf("[BLE] Sending: %s\n", buffer);
  #endif
  
  txCharacteristic.writeValue(buffer, strlen(buffer));
}

// ======================= LED FEEDBACK =======================
#if LED_FEEDBACK
static void blinkLED(int pin, int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(pin, HIGH);
    delay(delayMs);
    digitalWrite(pin, LOW);
    delay(delayMs);
  }
}

static void setStatusLED(bool listening, bool detected) {
  if (detected) {
    digitalWrite(LEDB, LOW);   // Blue LED on for detection
    digitalWrite(LEDG, HIGH);
    digitalWrite(LEDR, HIGH);
  } else if (listening) {
    digitalWrite(LEDG, LOW);   // Green LED on when listening
    digitalWrite(LEDB, HIGH);
    digitalWrite(LEDR, HIGH);
  } else {
    digitalWrite(LEDR, LOW);   // Red LED when not ready
    digitalWrite(LEDG, HIGH);
    digitalWrite(LEDB, HIGH);
  }
}
#endif

// ======================= NDP120 CALLBACK =======================
// This gets called when the NDP120 detects a keyword
static volatile bool keywordDetected = false;
static volatile int detectedKeywordIndex = -1;

void ndpEventCallback(void) {
  // NDP120 detected something
  int idx = NDP.poll();
  if (idx >= 0 && idx < TOKEN_COUNT) {
    detectedKeywordIndex = idx;
    keywordDetected = true;
  }
}

// ======================= SETUP =======================
void setup() {
  #if DEBUG_SERIAL
  Serial.begin(115200);
  while (!Serial && millis() < 3000);  // Wait up to 3 seconds for serial
  Serial.println("\n[BOOT] Nicla Voice Timer Controller Starting...");
  #endif
  
  #if LED_FEEDBACK
  pinMode(LEDR, OUTPUT);
  pinMode(LEDG, OUTPUT);
  pinMode(LEDB, OUTPUT);
  setStatusLED(false, false);
  #endif
  
  // Initialize NDP120
  #if DEBUG_SERIAL
  Serial.println("[NDP] Initializing NDP120...");
  #endif
  
  if (!NDP.begin("timer_voice_model.bin")) {
    #if DEBUG_SERIAL
    Serial.println("[NDP] ERROR: Failed to initialize NDP120!");
    Serial.println("[NDP] Make sure the model file is loaded");
    #endif
    
    // Blink red LED to indicate error
    #if LED_FEEDBACK
    while (true) {
      blinkLED(LEDR, 3, 200);
      delay(1000);
    }
    #endif
  }
  
  NDP.onEvent(ndpEventCallback);
  
  #if DEBUG_SERIAL
  Serial.println("[NDP] NDP120 initialized successfully");
  #endif
  
  // Initialize BLE
  #if DEBUG_SERIAL
  Serial.println("[BLE] Initializing BLE...");
  #endif
  
  if (!BLE.begin()) {
    #if DEBUG_SERIAL
    Serial.println("[BLE] ERROR: Failed to initialize BLE!");
    #endif
    
    #if LED_FEEDBACK
    while (true) {
      blinkLED(LEDR, 5, 100);
      delay(1000);
    }
    #endif
  }
  
  BLE.setLocalName("NiclaVoice_Timer");
  BLE.setAdvertisedService(uartService);
  uartService.addCharacteristic(txCharacteristic);
  uartService.addCharacteristic(rxCharacteristic);
  BLE.addService(uartService);
  
  // Start scanning for the Timer Device (Qualia)
  BLE.scan();
  
  #if DEBUG_SERIAL
  Serial.println("[BLE] Scanning for TimerDevice...");
  #endif
  
  #if LED_FEEDBACK
  setStatusLED(true, false);
  #endif
  
  #if DEBUG_SERIAL
  Serial.println("[BOOT] Ready! Listening for voice commands...");
  #endif
}

// ======================= BLE CONNECTION MANAGEMENT =======================
static BLEDevice peripheral;

static void connectToTimerDevice() {
  if (bleConnected) return;
  
  BLEDevice found = BLE.available();
  
  if (found) {
    String name = found.localName();
    
    #if DEBUG_SERIAL
    Serial.printf("[BLE] Found device: %s\n", name.c_str());
    #endif
    
    if (name == targetDeviceName) {
      BLE.stopScan();
      
      #if DEBUG_SERIAL
      Serial.println("[BLE] Connecting to TimerDevice...");
      #endif
      
      if (found.connect()) {
        #if DEBUG_SERIAL
        Serial.println("[BLE] Connected!");
        #endif
        
        if (found.discoverAttributes()) {
          BLECharacteristic characteristic = found.characteristic(CHARACTERISTIC_UUID_RX);
          if (characteristic) {
            peripheral = found;
            bleConnected = true;
            
            #if LED_FEEDBACK
            blinkLED(LEDG, 3, 100);  // Success indication
            #endif
            
            return;
          }
        }
        
        // Failed to discover services
        #if DEBUG_SERIAL
        Serial.println("[BLE] Failed to discover services");
        #endif
        found.disconnect();
      } else {
        #if DEBUG_SERIAL
        Serial.println("[BLE] Failed to connect");
        #endif
      }
      
      // Resume scanning
      BLE.scan();
    }
  }
}

static void checkBLEConnection() {
  if (bleConnected) {
    if (!peripheral.connected()) {
      #if DEBUG_SERIAL
      Serial.println("[BLE] Disconnected from TimerDevice");
      #endif
      bleConnected = false;
      BLE.scan();
    }
  }
}

// ======================= TOKEN PROCESSING =======================
static void processDetectedToken(int tokenIndex) {
  if (tokenIndex < 0 || tokenIndex >= TOKEN_COUNT) return;
  
  #if DEBUG_SERIAL
  Serial.printf("[KWS] Detected: %s (token %d)\n", TOKEN_NAMES[tokenIndex], tokenIndex);
  #endif
  
  #if LED_FEEDBACK
  setStatusLED(true, true);
  delay(100);
  setStatusLED(true, false);
  #endif
  
  // Add token to parse state
  if (parseState.count < MAX_TOKENS) {
    parseState.tokens[parseState.count++] = (uint8_t)tokenIndex;
    parseState.lastTokenTime = millis();
    
    // Check if this is a single-word command
    if (tokenIndex == TOK_STOP) {
      parseState.commandComplete = true;
    }
  }
}

static void processCommandTimeout() {
  if (parseState.count == 0) return;
  
  uint32_t now = millis();
  
  if (parseState.commandComplete || 
      (now - parseState.lastTokenTime > COMMAND_TIMEOUT_MS)) {
    
    // Parse the accumulated tokens
    TimerCommand cmd = parseCommand(parseState);
    
    if (cmd.type != CMD_NONE) {
      #if DEBUG_SERIAL
      Serial.printf("[CMD] Type: %d, Name: %s, Duration: %lu\n", 
                    cmd.type, cmd.name, (unsigned long)cmd.durationSeconds);
      #endif
      
      // Send command via BLE
      sendCommand(cmd);
      
      #if LED_FEEDBACK
      blinkLED(LEDB, 2, 100);  // Command sent indication
      #endif
    }
    
    // Reset parse state
    parseState.count = 0;
    parseState.commandComplete = false;
  }
}

// ======================= MAIN LOOP =======================
void loop() {
  // BLE connection management
  BLE.poll();
  
  if (!bleConnected) {
    connectToTimerDevice();
  } else {
    checkBLEConnection();
  }
  
  // Process keyword detection from NDP120
  if (keywordDetected) {
    keywordDetected = false;
    processDetectedToken(detectedKeywordIndex);
  }
  
  // Check for command timeout (parse accumulated tokens)
  processCommandTimeout();
  
  // Serial command interface for testing (without NDP120)
  #if DEBUG_SERIAL
  while (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    if (input.startsWith("sim ")) {
      // Simulate a token: "sim set" or "sim 0"
      String tokenStr = input.substring(4);
      
      int tokenIdx = -1;
      
      // Try to parse as number first
      tokenIdx = tokenStr.toInt();
      
      // If that didn't work, search by name
      if (tokenIdx == 0 && tokenStr != "0") {
        for (int i = 0; i < TOKEN_COUNT; i++) {
          if (tokenStr.equalsIgnoreCase(TOKEN_NAMES[i])) {
            tokenIdx = i;
            break;
          }
        }
      }
      
      if (tokenIdx >= 0 && tokenIdx < TOKEN_COUNT) {
        processDetectedToken(tokenIdx);
      } else {
        Serial.printf("Unknown token: %s\n", tokenStr.c_str());
      }
    } else if (input == "status") {
      Serial.printf("BLE connected: %s\n", bleConnected ? "yes" : "no");
      Serial.printf("Parse buffer: %d tokens\n", parseState.count);
    } else if (input == "tokens") {
      Serial.println("Available tokens:");
      for (int i = 0; i < TOKEN_COUNT; i++) {
        Serial.printf("  %2d: %s\n", i, TOKEN_NAMES[i]);
      }
    } else if (input == "help") {
      Serial.println("\n=== Commands ===");
      Serial.println("  sim <token>  - Simulate token detection");
      Serial.println("  status       - Show connection status");
      Serial.println("  tokens       - List all tokens");
      Serial.println("");
      Serial.println("Example: sim set, sim five, sim minutes");
    }
  }
  #endif
  
  delay(10);
}
