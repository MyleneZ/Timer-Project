# Tivvy RPi Voice Bridge

Replaces the Nicla Voice with a Raspberry Pi 4 doing full offline dictation, and
funnels recognized commands to the Qualia over BLE (or USB serial).

The Pi speaks the **same wire protocol the Nicla used**, so the Qualia keeps its
existing `parseCommand()`:

```
CMD:SET,NAME:Baking,DURATION:1200
CMD:CANCEL,NAME:Baking
CMD:ADD,NAME:Baking,DURATION:60
CMD:MINUS,NAME:Baking,DURATION:60
CMD:STOP
```

Nothing leaves the device: Vosk runs locally, so the offline/privacy claim in
the paper still holds — it just moves from the NDP120 to the Pi's CPU.

---

## Pipeline

```
USB mic ─▶ 16 kHz mono frames ─▶ VAD segmentation ─▶ Vosk (grammar-constrained)
                                                        │
                                              text ─────┘
                                                        ▼
                                     NLU  "set a baking timer for 20 minutes"
                                                        ▼
                                     shadow state (names, slots, ordinals)
                                                        ▼
                                     CMD:SET,NAME:Baking,DURATION:1200
                                                        ▼
                                     BLE central ─▶ Qualia NUS RX characteristic
```

| Module | Job |
|---|---|
| `audio.py` | PortAudio capture, WebRTC/energy VAD, utterance segmentation |
| `asr.py` | Vosk (default) or faster-whisper, both fully offline |
| `nlu.py` | Text → command. Numbers, units, names, fuzzy recovery |
| `state.py` | Shadow copy of the Qualia's 3 timer slots, for name resolution |
| `protocol.py` | The wire format and every firmware constraint it implies |
| `transport.py` | BLE central (`bleak`), USB serial (`pyserial`), or stdout |
| `app.py` | Wiring, wake gating, duplicate suppression, logging |

---

## Hardware

* Raspberry Pi 4 (2 GB is enough; the small Vosk model needs ~200 MB RSS)
* A USB microphone. A cheap USB conference mic beats a bare electret — the
  Pi has no analog input, and mic quality dominates recognition accuracy.
* The Pi's built-in Bluetooth (no dongle needed)
* The Qualia running `device_code/production.ino`

The Pi does **not** need a speaker or display: the Qualia still owns all audio
and visual feedback.

---

## Install

```bash
git clone <this repo> && cd Timer-Project/rpi_bridge
sudo ./scripts/install.sh
```

That installs system packages, creates `/opt/tivvy/venv`, downloads the Vosk
model to `/opt/tivvy/models`, writes `/etc/tivvy/bridge.toml`, and enables the
`tivvy-bridge` systemd unit (without starting it).

Then:

```bash
# 1. Which mic?
/opt/tivvy/venv/bin/python -m tivvy_bridge --list-audio
#    → set [audio] device in /etc/tivvy/bridge.toml

# 2. Where is the Qualia? (power it on first)
/opt/tivvy/venv/bin/python -m tivvy_bridge --scan-ble
#    → paste the address into [link] device_address

# 3. Does everything load?
/opt/tivvy/venv/bin/python -m tivvy_bridge --config /etc/tivvy/bridge.toml --check

# 4. Send a command without speaking
/opt/tivvy/venv/bin/python -m tivvy_bridge --config /etc/tivvy/bridge.toml \
    --say "set a baking timer for two minutes"

# 5. Run it
sudo systemctl start tivvy-bridge
journalctl -u tivvy-bridge -f
```

### Manual install

```bash
sudo apt install -y python3-venv python3-dev libportaudio2 portaudio19-dev bluez curl unzip
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
./scripts/download_vosk_model.sh ~/tivvy-models
cp config.example.toml bridge.toml   # then edit asr.model_path
python -m tivvy_bridge --config bridge.toml
```

---

## Supported speech

| Say | Sends |
|---|---|
| "set a timer called baking for twenty minutes" | `CMD:SET,NAME:Baking,DURATION:1200` |
| "start a fifteen minute homework timer" | `CMD:SET,NAME:Homework,DURATION:900` |
| "create an exercise timer for half an hour" | `CMD:SET,NAME:Exercise,DURATION:1800` |
| "add five more minutes to the baking timer" | `CMD:ADD,NAME:Baking,DURATION:300` |
| "take ten minutes off exercise" | `CMD:MINUS,NAME:Exercise,DURATION:600` |
| "cancel the break timer" / "cancel timer two" | `CMD:CANCEL,NAME:Break` |
| "stop" | `CMD:STOP` |

Names that get themed art on the Qualia: **Baking, Cooking, Break, Homework,
Exercise, Workout** (plus aliases — "gym" → Workout, "study" → Homework, and so
on; see `NAME_ALIASES` in `nlu.py`). Any other name still works, it just renders
with the default theme.

Deliberate safety behaviours:

* A `SET` with no spoken duration is **rejected**, not defaulted. A misheard
  duration is worse than no timer. Change `[nlu] set_without_duration_seconds`
  if you disagree.
* `CANCEL`/`ADD`/`MINUS` with no name only auto-target when exactly one timer
  is running. With two or more, the command is dropped rather than guessed.
* An identical command inside `duplicate_window_s` is ignored, which is what
  keeps the Qualia's own alarm audio from re-triggering a command.

---

## Tuning accuracy

1. **Keep the grammar on.** `[asr] use_grammar = true` restricts the decoder to
   the 138-word Tivvy vocabulary. This is the single biggest win.
2. **Log transcripts.** Set `transcript_log`, run a session, then read the file:
   every utterance appears with its confidence and what it parsed to.
3. **Adjust `min_confidence`.** Raise it if background conversation triggers
   commands; lower it if the device ignores the user.
4. **Adjust the VAD.** If words get clipped, raise `preroll_ms` and
   `end_silence_ms`. In a noisy room, raise `aggressiveness` to 3.
5. **Try Whisper.** `[asr] backend = "whisper"` with `tiny.en` handles unusual
   phrasing and accents better, at ~1-3 s per utterance instead of ~0.2 s.
   `pip install faster-whisper numpy` first.
6. **Wake word.** `[wake] enabled = true` makes an utterance require a wake
   phrase. Caveat: with `use_grammar = true`, wake phrases must be real English
   words that exist in the model's lexicon — an invented word like "Tivvy" is
   out-of-vocabulary and will never be recognized. Use "hey timer" instead, or
   turn the grammar off.

---

## Firmware side

`device_code/production.ino` has already been updated for this bridge — flash it
before running the Pi. What changed:

* `INDEPENDENT_DEMO` is now `false`. The demo queue used to create and cancel
  timers on its own schedule for the first 45 s after boot and would fight
  every command the Pi sends. **If you re-enable it for a demo, turn it back
  off afterwards.**
* BLE writes are queued, not executed in the callback. `onWrite` now only
  enqueues the string; `loop()` drains it. Previously the callback mutated
  timers, poked the I2C backlight expander and started audio from the NimBLE
  host task, concurrently with the render loop.
* `NimBLEDevice::setMTU(247)`, plus an explicit advertised name in the scan
  response so name-based discovery is deterministic.
* `findTimerByName` is case-insensitive, so a capitalisation difference can no
  longer turn cancel/add/minus into a silent no-op.
* Zero-duration `SET`, duplicate names and a `MINUS` past zero are all handled;
  none of them can wedge a timer slot any more.
* Refused commands play a distinct error tone instead of failing silently.
* The TX characteristic now emits `ACK:` and `STATE:` lines, which this bridge
  consumes (`state.apply_device_state`) so its timer mirror is real state
  rather than an inference.
* The serial handler understands the full `CMD:` protocol, so
  `transport = "serial"` works without further changes.

---

## Troubleshooting

| Symptom | Check |
|---|---|
| `no Tivvy display found in scan results` | Qualia powered on? `bluetoothctl scan on`. Pin `device_address`. |
| Connects, but commands do nothing | Is `INDEPENDENT_DEMO` still `true`? Watch the Qualia's serial log for `[BLE] Received:`. |
| `CANCEL`/`ADD` silently ignored by the display | Name case mismatch — the firmware's `findTimerByName` is `strcmp`. Apply the case-insensitive fix. |
| Recognizes nothing | `--list-audio`, confirm the right device; `arecord -d 3 test.wav && aplay test.wav`. |
| Recognizes everything as commands | Raise `[asr] min_confidence`, or enable the wake word. |
| High CPU | Confirm `use_grammar = true` and that you are on the *small* model. |

---

## Development

```bash
pip install pytest
pytest                                   # 70 tests, no hardware needed
python -m tivvy_bridge --dry-run --say "set a baking timer for twenty minutes"
```

`--dry-run` prints commands instead of transmitting them; `--say` skips the
microphone and the ASR model entirely, so the parser can be exercised anywhere.
