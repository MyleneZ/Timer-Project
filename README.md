# Timer-Project

Interaction Notes:
- player sounds in response to commands

Commands List
- Set a timer for how long before ringing
    - "Set a timer called [name] for + [minute/hours]."
- Cancel the timer
    - "Cancel the timer called [name]."
- Add time to an existing timer
    - "Add [hours/minutes] to [name] timer."
- Subtract time from an existing timer
    - "Minus [hours/minutes] to [name] timer."
- Stop the timer ringtone
    - "Stop!"
    - auto-shutoff after 90 seconds

Bootup Behaviour
- show on the display that no timers are currently running

SFX List
- alarm ringtone
- cancel ping
- bootup sound
- confirm ping (e.g. when a timer is set)



Arduino IDE Libraries (install via Tools -> Manage Libraries...)
- LovyanGFX (for the display); alternative is Arduino_GFX
- NimBLE-Arduino (BLE capabilities)
- ArduinoJson (sending data to flutter)

https://learn.adafruit.com/adafruit-qualia-esp32-s3-for-rgb666-displays/arduino-ide-setup

/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino: In function 'void setup()':
/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino:4:11: error: 'LED_BUILTIN' was not declared in this scope
    4 |   pinMode(LED_BUILTIN, OUTPUT);
      |           ^~~~~~~~~~~
/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino: In function 'void loop()':
/private/var/folders/j1/441zhjvd7vq9fncs7y4vlbrh0000gn/T/.arduinoIDE-unsaved202579-22963-61i3ie.zqnz6/BareMinimum/BareMinimum.ino:11:16: error: 'LED_BUILTIN' was not declared in this scope
   11 |   digitalWrite(LED_BUILTIN, HIGH);   // turn the LED on (HIGH is the voltage level)
      |                ^~~~~~~~~~~
exit status 1

Compilation error: 'LED_BUILTIN' was not declared in this scope