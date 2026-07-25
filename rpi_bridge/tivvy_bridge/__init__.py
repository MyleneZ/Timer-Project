"""Tivvy RPi voice bridge.

Runs on a Raspberry Pi 4 and replaces the Nicla Voice in the Tivvy stack:
microphone -> offline speech recognition -> command parsing -> Qualia.

The wire format sent to the Qualia is unchanged from the Nicla firmware, so the
ESP32-S3 side keeps its existing ``parseCommand()`` implementation:

    CMD:SET,NAME:Baking,DURATION:1200
    CMD:CANCEL,NAME:Baking
    CMD:ADD,NAME:Baking,DURATION:60
    CMD:MINUS,NAME:Baking,DURATION:60
    CMD:STOP
"""

__version__ = "1.0.0"
