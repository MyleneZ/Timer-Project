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

Train Data Script:
- "Set a timer called Timer 1 for 20 minutes."
- "Cancel the timer called Timer 1."
- "Add 5 minutes to Timer 1."
- "Minus 5 minutes to Timer 1."
- "Stop"
- "One, Two, Three, Four, Five, Six, Seven, Eight, Nine, Minutes, Hours"
- "Ten, Eleven, Twelve, Thirteen, Fourteen, Fifteen, Sixteen, Seventeen, Eighteen, Nineteen"
- "Twenty, Thirty, Forty, Fifty, Sixty, Seventy, Eighty, Ninety"
- "Baking, Break, Homework, Exercise, Workout"



Bootup Behaviour
- show on the display that no timers are currently running

SFX List
- alarm ringtone
- cancel ping
- bootup sound
- confirm ping (e.g. when a timer is set)
starting point for sources: https://pixabay.com/sound-effects/



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




Voice Command Logic
- detect when a command starts and ends
    - several periods are either silence or have constant noise --> nothing is happening
    - random spikes and dips in audio --> potential command
- determine if the command is one we care about
    - use the audio sources to create a "footprint" of an expected command
    - take the detected potential command and determine if a section matches any footprint
        - we use "distance" to the expected footprint to determine what command if any
    - word soup approach: this allows us to accept both "set a timer called X for Y" and "set X for Y"
- act on the commands
    - Design Notes:
        - if all three timers are active, ignore new set command but accept stops (must at least say the number of which timer to stop OR the name)
        - when setting a timer name, check if one of the supported key terms is within the segment and set the timer name accordingly
            - if none, default to Timer X (X changes based on placement to avoid confusion)
