# Firmware (Arduino, ESP32)

| Sketch | Purpose |
|---|---|
| [`esp32_voice_assistant/`](esp32_voice_assistant/) | **The firmware running on the robot.** Streams the mic to the server, plays the answer, beeps for feedback. Unchanged from the tested April code except that credentials moved to `secrets.h`; its log lines still say "8kHz" although it runs at 16 kHz. |
| [`experimental/esp32_voice_assistant_next/`](experimental/) | The same firmware with review fixes (auth token, mic un-mute safety net, cleaner end of answer). Not yet tested on hardware. |
| [`hardware_tests/speaker_beep_test/`](hardware_tests/speaker_beep_test/) | No WiFi. Beeps through the MAX98357A so you can check the speaker wiring. |
| [`hardware_tests/mic_stream_test/`](hardware_tests/mic_stream_test/) | Streams the INMP441 to the server and nothing else. Watch the server's VAD bar to check the mic. Needs its own `secrets.h` (copy the example in that folder). |

## One-time setup in the Arduino IDE

1. Install the **ESP32 board package**: *File > Preferences > Additional boards
   manager URLs* -> `https://espressif.github.io/arduino-esp32/package_esp32_index.json`,
   then *Tools > Board > Boards Manager* -> "esp32 by Espressif Systems".
   The code uses the classic `driver/i2s.h` API, which is available in core 2.x and
   still present (marked deprecated) in 3.x.
2. Install the library **"WebSockets" by Markus Sattler** (Library Manager). That is
   the only library needed; the JSON is parsed by hand to save RAM.
3. Select **Tools > Board > "DOIT ESP32 DEVKIT V1"** (or "ESP32 Dev Module"),
   upload speed 921600, and the COM port of the board.

## Flashing the voice assistant

1. Copy `esp32_voice_assistant/secrets.h.example` to `esp32_voice_assistant/secrets.h`
   and fill in your WiFi name/password, the server IP/port and the auth token.
   `secrets.h` is git-ignored.
2. Open `esp32_voice_assistant/esp32_voice_assistant.ino`, click Upload.
3. Open the Serial Monitor at **115200 baud**. A healthy boot looks like:

```
[SPK] ✅ Speaker ready (16000 Hz, 16-bit mono)
[MIC] ✅ Microphone ready (16000 Hz, 32-bit I2S -> 16-bit PCM)
[WiFi] ✅ Connected! IP=192.168.1.42
[WS] ✅ CONNECTED: /
[WS] ✅ ESP32 REGISTERED - streaming microphone at 16000 Hz
```

The blue LED turns **off** when the robot is registered with the server.

## Knobs you may want to turn (top of the .ino)

| Define | Default | Effect |
|---|---|---|
| `AUDIO_SAMPLE_RATE` | 16000 | Must match `Config.SampleRate` in the server. 8000 halves the bandwidth on a weak hotspot (the server must be changed too). |
| `MIC_GAIN_MULTIPLIER` | 1.5 | Mic loudness sent to the server. |
| `SPK_VOLUME_GAIN` | 3.0 | Software volume of the answer (clipped at full scale). |
| `BEEP_ENABLED` | true (main), false (experimental) | A faint 800 Hz beep every 2 s, useful while checking wiring. The main firmware has it on, as it was flashed. |
| `MIC_PAUSE_TIMEOUT_MS` | 20000 (experimental only) | Safety net: un-mute the mic if the server never answers after a wake beep. |
| `WS_HEARTBEAT_*` | 15 s / 30 s / 3 | Ping/pong to detect a dead server and reconnect. |

## How the firmware is organised

- `loop()` (core 1) reads the I2S mic, converts 32-bit frames to 16-bit, and pushes
  chunks into `micSendQueue`; it also pumps the WebSocket and drains `wsSendQueue`.
- `microphoneSendTask` (core 0) batches 3 chunks, base64-encodes them into one JSON
  message and queues it for sending.
- `onWebSocketEvent` parses incoming JSON with a tiny hand-written parser, plays the
  feedback beeps directly, and pushes decoded TTS chunks into `spkPlayQueue`.
- `speakerPlaybackTaskFunction` (core 0) applies `SPK_VOLUME_GAIN` and writes the
  chunks to the I2S amplifier; after the last chunk it un-mutes the microphone.
