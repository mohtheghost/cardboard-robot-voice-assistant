# ESP32 <-> server protocol

One WebSocket connection, plain `ws://`, text frames only, every frame is a JSON
object. Audio travels as base64 inside the JSON. The ESP32 is the client; the
server (`server/Program.cs`) listens on port 8080 (`ROBOT_PORT`).

Audio format in both directions: **16 000 Hz, 16-bit signed little-endian, mono
PCM**. `AUDIO_SAMPLE_RATE` in the firmware and `Config.SampleRate` in the server
must always match.

## ESP32 -> server

| When | Message | Notes |
|---|---|---|
| Right after connecting | `{"id":"esp32"}` or `{"id":"esp32","token":"<ROBOT_AUTH_TOKEN>"}` | Registration. If the server has `ROBOT_AUTH_TOKEN` set and the token is missing or wrong, the socket is closed with code 1008. Only the experimental firmware sends a token; the main firmware sends the plain form, so leave `ROBOT_AUTH_TOKEN` unset when using it. |
| Continuously while the mic is enabled | `{"type":"audio","data":"<base64>"}` | 1536 samples (3072 bytes, 96 ms) per message, about 10 messages per second. Only the `data` field is used by the server. |

The mic is **not** streamed while the robot is playing an answer, while the
WebSocket is down, or while it is muted by a `wake_beep` (see below).

## server -> ESP32

| Message | Meaning | What the ESP32 does |
|---|---|---|
| `{"target":"esp32","type":"speech_ended"}` | The VAD decided your sentence is over (at least 500 ms of speech followed by ~1 s of silence). | Plays 3 quick beeps. |
| `{"target":"esp32","type":"wake_beep"}` | Whisper transcribed the sentence and it contains a wake word. An answer is coming. | Plays 1 beep and **mutes the microphone**. |
| `{"target":"esp32","type":"tts","chunk":i,"total":n,"data":"<base64>"}` | One chunk of the spoken answer: 2048 bytes of PCM (64 ms). Sent every 15 ms, so faster than real time; `chunk` counts from 0. | Decodes and queues it; playback starts on the first chunk. After the last chunk it un-mutes the microphone. |
| `{"target":"esp32","type":"listen"}` | A wake word was heard but no answer will follow (empty command, ChatGPT or TTS error). | Experimental firmware: un-mutes the microphone immediately. Main firmware: ignores it (and stays muted until reboot; see `docs/release-checklist.md`). |

The server also ignores the robot's audio for 0.7 s after sending `speech_ended`
or `wake_beep`, because the robot's microphone hears its own beeps.

The `target` field is ignored by the firmware; it is a leftover from an older
setup where a PC and an ESP32 shared one relay server.

## Full round trip

```
you speak ──► ESP32 mic ──► {"type":"audio"} x N ──► server VAD
                                                      │  silence detected
      3 beeps ◄── speech_ended ◄──────────────────────┘
                                                      ▼
                                         WAV ──► Whisper ──► "hello robot, tell me a joke"
                                                      │ wake word found
      1 beep, mic muted ◄── wake_beep ◄───────────────┘
                                                      ▼
                                         ChatGPT ──► TTS mp3 ──► ffmpeg ──► PCM16
                                                      │
      speaker plays ◄── tts chunk 0..n-1 ◄────────────┘
      mic un-muted after the last chunk
```

If anything fails after the wake beep the server sends `listen`; the experimental
firmware acts on it and also un-mutes by itself after `MIC_PAUSE_TIMEOUT_MS`
(20 s) if nothing arrives at all.

## Sizes and limits worth knowing

| Value | Where | Why it matters |
|---|---|---|
| `WEBSOCKETS_MAX_DATA_SIZE = 24 KB` | firmware, before including the WebSockets library | A TTS frame is ~2.8 KB of base64 plus JSON; the default library limit is smaller than some frames. |
| `TTS_B64_WORK_SIZE = 12288` | firmware | Scratch buffer for one TTS chunk's base64 (needs > 2732 bytes). |
| `SPK_PLAY_QUEUE_SIZE = 16` | firmware | ~1 s of buffered answer. When full, the WebSocket callback waits, which throttles the server through TCP. |
| `chunkSize = 2048` bytes, `Task.Delay(15)` | server `SendAudioToEsp32Async` | Streaming pace. |
| VAD: noise + 1000 offset, 3 s calibration, start after 2 chunks, stop after 10 silent chunks, min 500 ms | server `Config` | Tune `VadNoiseOffset` if the robot never triggers (too high) or triggers on nothing (too low). Calibration happens once per connection, so keep quiet for 3 s after the ESP32 connects. |
| Heartbeat ping every 15 s, timeout 30 s, 3 retries | firmware | Detects a dead server and reconnects every 500 ms. |
