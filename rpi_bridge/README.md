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

* Raspberry Pi 4, or a **Pi Zero 2 W** — see below
* A USB microphone. A cheap USB conference mic beats a bare electret — the
  Pi has no analog input, and mic quality dominates recognition accuracy.
* The Pi's built-in Bluetooth (no dongle needed)
* The Qualia running `device_code/production.ino`

The Pi does **not** need a speaker or display: the Qualia still owns all audio
and visual feedback.

### Pi 4 vs Pi Zero 2 W

A Zero 2 W runs this fine on the **default config** (Vosk small + grammar), and
its size suits the enclosure better. Measured footprint of the loaded
grammar-constrained model is **~155 MB RSS**, so 512 MB is enough on Raspberry
Pi OS **Lite** — with the desktop image it is too tight.

|  | Pi 4 | Pi Zero 2 W |
|---|---|---|
| CPU | Cortex-A72 @ 1.5 GHz | Cortex-A53 @ 1.0 GHz, in-order |
| RAM | 2–8 GB | 512 MB |
| Vosk small + grammar | comfortable | works; expect a longer decode after each utterance |
| `backend = "whisper"` | usable (1–3 s) | **no** — too slow, and int8 `tiny.en` will not fit alongside Vosk |
| USB mic | plug into a USB-A port | needs a **micro-USB OTG adapter**, or use an I2S MEMS mic on the GPIO header |
| Bluetooth | 5.0 | 4.2 — fine for NUS, but it shares its antenna with Wi-Fi |

Take the Zero 2 W if you want the small enclosure and stay on Vosk. Take the Pi
4 if you want the Whisper backend as an escape hatch when Vosk is not accurate
enough for a given speaker, or headroom to run anything else on the same board.

On a Zero 2 W: use the Lite image, leave `use_grammar = true`, and keep Wi-Fi
idle in normal operation (everything is offline anyway) so it does not contend
with BLE for the shared radio.

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
pytest                                   # 91 tests, no hardware needed
python -m tivvy_bridge --dry-run --say "set a baking timer for twenty minutes"
```

`--dry-run` prints commands instead of transmitting them; `--say` skips the
microphone and the ASR model entirely, so the parser can be exercised anywhere.

### Smoke testing on a laptop

Everything except the BLE link runs on macOS, so the pipeline can be proven
before anything reaches the Pi. `sounddevice`, `vosk`, `bleak` and `webrtcvad`
are all imported lazily, so `--say` needs no dependencies at all.

```bash
python3.12 -m venv .venv                 # 3.11–3.13; vosk has no 3.14 wheel yet
.venv/bin/pip install -r requirements.txt
./scripts/download_vosk_model.sh ~/tivvy-models
cp config.example.toml bridge.mac.toml   # then point [asr] model_path at ~/tivvy-models
                                         # and set [link] transport = "stdout"

.venv/bin/python -m tivvy_bridge --config bridge.mac.toml --check
.venv/bin/python scripts/smoke_test.py -c bridge.mac.toml
.venv/bin/python scripts/smoke_test.py -c bridge.mac.toml --chatter
```

`scripts/smoke_test.py` synthesizes speech with macOS `say` (or `espeak-ng` on
Linux) and pushes it through the real VAD, Vosk, NLU and link, so it exercises
the whole chain without anyone having to talk to the laptop. `--chatter` speaks
ordinary conversation instead and counts how much of it gets forced into a
command — the number to watch when tuning `min_confidence` or deciding whether
to turn the wake word on.

### Driving the display without a microphone

`scripts/send_command.py` sends the wire protocol straight to the Qualia and
prints the `ACK:` / `STATE:` notifications that come back, so you can see what
the firmware did with each command instead of guessing from across the room.
This is the fastest way to separate a firmware problem from a voice problem.

```bash
./scripts/send_command.py --demo                    # scripted bring-up run
./scripts/send_command.py                           # interactive prompt
./scripts/send_command.py "CMD:SET,NAME:Baking,DURATION:120"
./scripts/send_command.py --say "set a baking timer for two minutes"
./scripts/send_command.py --listen 10               # just watch notifications
./scripts/send_command.py --transport stdout --demo # rehearse with no hardware
```

The interactive prompt takes either form:

```
tivvy> set a baking timer for two minutes
   -> CMD:SET,NAME:Baking,DURATION:120
      <- ACK:SET,NAME:Baking,OK:1
      <- timers: Baking 02:00
```

`--demo` is a conformance run, not just a sequence of commands: it deliberately
asks for a fourth timer when `MAX_TIMERS` is 3, subtracts past zero, reuses a
name, cancels something that does not exist, and sends a lowercase name to
confirm `findTimerByName` is case-insensitive. Each step declares whether the
firmware is expected to accept or refuse it and reports a **MISMATCH** if the
`ACK:` disagrees, so a firmware regression shows up as a failed step rather than
as odd behaviour weeks later.

### Testing with your own voice

```bash
./scripts/mic_test.py                  # print commands, no hardware needed
./scripts/mic_test.py --list           # which input devices exist
./scripts/mic_test.py --device "USB"   # pick one by index or name substring
./scripts/mic_test.py --transport ble  # actually drive the Qualia
./scripts/mic_test.py --wake           # require a wake phrase first
```

No venv activation needed — the script re-executes itself under `./.venv` if it
is not already running there.

It shows a live input-level meter while idle:

```
  [##########--------------------]  -34.2 dBFS  peak  -8.1  SPEECH
   2.1s  conf 0.96  'set a baking timer for twenty minutes'
          -> CMD:SET,NAME:Baking,DURATION:1200
             timers: Baking 00:19:59
```

The meter is the point. A mic that is not working and a mic that is working but
not recognizing you produce identical silence on stdout; the meter tells them
apart at a glance. If the bar never moves, it is permissions or the wrong
device, not the ASR — macOS needs your terminal listed under System Settings →
Privacy & Security → Microphone. Rejected utterances print the reason the NLU
gave, which is what you want when tuning phrasing.

Two macOS-specific caveats:

* **BLE addresses are not MAC addresses.** CoreBluetooth hands out a per-host
  UUID, so an address from `--scan-ble` on a Mac is meaningless in the Pi's
  `device_address`. Pin that value from a scan run *on the Pi*.
* The terminal needs Bluetooth permission (System Settings → Privacy &
  Security → Bluetooth) before `--scan-ble` returns anything.
