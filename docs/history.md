# How the project evolved

The robot was built between February and April 2026 by iterating with an AI
assistant: each attempt was downloaded as a new `.ino` file, flashed, and the
next one fixed whatever went wrong. Sixty-one of those files survived in one flat
folder. This page reconstructs the story from their timestamps and contents; the
files themselves are in [`../archive/firmware-versions/`](../archive/firmware-versions/)
and indexed in [`../archive/README.md`](../archive/README.md).

## Timeline at a glance

| When | What happened |
|---|---|
| Feb 10-18 | First experiments on the PC (`experiments/whisper-test`): raw PCM to WAV, an OpenAI TTS test, then posting a WAV to Whisper. |
| Feb 17 | The server project is created (retargeted from .NET 9 to .NET 8 the same day, to match the VPS). |
| Feb 20-22 | First firmware: microphone and speaker in one sketch. The two-I2S-port pin layout used ever since is chosen on Feb 20. |
| Feb 21-22 | Debugging by isolation: speaker-only test, mic-only test, then merging the two "known working" halves. |
| Feb 22-24 | The big "production" firmware (1,100-1,500 lines, ArduinoJson): 25 versions fighting WebSocket drops during playback, memory, an MCLK boot problem, then a diagnostics branch and an LED branch. |
| Mar 30 | The rewrite: stable WebSocket handling, 16 kHz audio, and a hand-written JSON parser replacing ArduinoJson. The March server is a plain relay between the ESP32 and a PC. |
| Mar 31 - Apr 1 | Buffer-then-play, jitter buffers, then plain streaming. Mic muting and the "speech ended" beeps are added on Apr 1. This is the code still running today. |
| Apr 3-7 | The server absorbs the PC's job (Whisper, ChatGPT, TTS) and becomes the unified VPS server. Photos and videos of the finished robot are taken on Apr 7. |
| Jun 3 | The edited demo video. |

## Architecture then and now

Until April the system had **three** parts: the ESP32, a relay server on the VPS,
and a PC program. The firmware still sends `"target":"pc"` in its audio messages
because the relay forwarded frames by that field, and a PC-side program did the
speech recognition and speech synthesis. Neither the relay nor the PC program
survived; the current server ignores `target` and does everything itself.

```
Feb-Mar 2026:   ESP32  <->  VPS relay  <->  PC (Whisper, ChatGPT, TTS)
Apr 2026 on:    ESP32  <->  VPS server (Whisper, ChatGPT, TTS)
```

## The families of firmware

### 1. First assistants (Feb 20-22)

`esp32_full_duplex.ino` tried one I2S port in duplex mode with the speaker on
GPIO 22. Twenty-four minutes later `esp32_dual_i2s_final.ino` split microphone
and speaker onto two ports with the pins still used today. The `_1.._3` copies
chased two problems: TTS frames that never seemed to arrive (debug tracing of
every WebSocket frame) and a 300 KB "buffer the whole answer, then play" malloc
that failed (queue and buffer shrunk, allocation sized from the largest free
block, `use_apll` turned off to cure an MCLK error).

`esp32_ultra_optimized.ino` was the breakthrough: instead of buffering the whole
answer it decodes each 8 KB chunk and plays it immediately. Every later firmware
kept that idea. `esp32_voice_assistant_FIXED.ino` closes the family with the
fix for "microphone not recording": the cross-core `playing` flag had to be
`volatile`.

### 2. Isolating each half (Feb 21-22, mostly after midnight)

When the combined sketches misbehaved, each half was tested alone:
`esp32_speaker_test.ino` (a beep, no WiFi), `esp32_simple_receiver.ino`
(speaker only, over WebSocket) and `esp32_simple_mic_test.ino` (microphone only).
The receiver lost its connection during playback because it waited inside the
WebSocket callback; `_fixed` removed the wait and added a heartbeat, which broke
playback; `esp32_hybrid_perfect.ino` put the wait back but pumped
`webSocket.loop()` inside it. Then came four near-identical merges of the two
"working" tests (`exact_combination`, `perfect_voice_assistant`,
`literal_combination`, `debug_version`) and finally `esp32_minimal_merge.ino`,
the two raw test loops side by side. Nine minutes after that the "production"
firmware appeared and this line was abandoned.

### 3. The "production" firmware (Feb 22-24)

`esp32_production_voice_assistant.ino` and its 15 numbered copies are one large
design: two FreeRTOS tasks, a send queue, a 30-second statistics report. The
morning session fixed hardware bring-up. The base version forgot the
`mck_io_num` pin field, `_1` added it in a way g++ rejects (designated
initialisers must follow declaration order), `_2` assigned it after the struct,
`_3` added a software volume gain (`_4` is a byte-identical re-download). The
afternoon fought playback: `_5` accumulated the whole answer (150 KB), `_6` added
guards, `_7` went back to per-chunk streaming.

The evening session (`_8` to `_15`) chased WebSocket drops during playback: the
callback blocked for up to 2 s per chunk while the heartbeat timeout was 3 s.
`_9` pumped the WebSocket inside the waits, relaxed the heartbeat, halved the
gain and switched off the brown-out detector because the USB power bank sagged.
`_11` tried a 3-chunk pre-buffer and did not compile, `_12` fixed it, `_13`
waited a full chunk, and `_14` found the sound model: wait only until the chunk
is in the I2S DMA buffer and let the hardware pace playback. `_15` bolted a
white-noise generator on as a wiring check.

Feb 23-24 produced the variants: `esp32_voice_assistant_FIXED_1..5` (a
different, 63 KB program despite the name; `FIXED_2` fixed silence when the
first chunk was not chunk 0, `FIXED_3` = `FIXED_4` dumped samples, `FIXED_5`
retuned everything for stability), a remote-diagnostics branch
(`ESP32_WITH_DIAGNOSTICS_COMPLETE`, `ESP32_PRODUCTION_WITH_DIAGNOSTICS`) that
sent log lines to a PC receiver that no longer exists, and the LED branch
(`ESP32_COMPLETE_WITH_LED.ino`, the last of the family; `ESP32_WITH_LED_INDICATOR.ino`
is a 233-byte download error, not code). `ESP32_ASYNC_TTS_FIXED.ino` (Feb 23)
is a stand-alone async-queue design that the March rewrite replaced.

### 4. The March rewrite (Mar 30)

Five weeks later `ESP32_FIXED_HEARTBEAT.ino` restarted from a clean base: a
15 s / 30 s heartbeat, a `wsSendQueue` so only `loop()` ever sends, a status LED
on GPIO 2, WiFi reconnect, and back-pressure instead of dropping TTS chunks.
`ESP32_16KHZ_HIGH_QUALITY.ino` doubled the sample rate to 16 kHz "for clearer
Arabic TTS", and `_1` enlarged the JSON document buffer. At midday
`ESP32_WITH_TEST_BEEP.ino` dropped ArduinoJson for a hand-written parser (the
first version did not compile: a `TWO_PI` constant collided with the ESP32
core's macro, fixed five minutes later) and added the periodic test beep.
`ESP32_WITH_WAKE_BEEP.ino` generalised the beep code and plays a confirmation
beep when the server sends `wake_beep`. It is the direct ancestor of everything
that followed.

### 5. Streaming, jitter buffers, and the current design (Mar 31 - Apr 5)

On a mobile hotspot the answers kept cutting out. `ESP32_BUFFER_THEN_PLAY.ino`
collected the whole answer (150 KB, with progress beeps at 25/50/75/100 %) and
failed to allocate; `_FIXED` shrank it to 80 KB; nine minutes later
`ESP32_STREAMING_OPTIMIZED.ino` went back to a play queue and a speaker task
("~4 KB instead of 80-164 KB"). `ESP32_JITTER_BUFFER.ino` added a 10-chunk
pre-buffer, `ESP32_ADAPTIVE_JITTER_BUFFER.ino` an adaptive re-buffering loop and
an 8 kHz rate to halve the bandwidth, and `ESP32_STREAMING_8KHZ.ino` removed the
jitter logic again the same evening.

`ESP32_STREAMING_8KHZ_1.ino` (Apr 1) added what a teacher's feedback asked for:
the microphone is muted from the wake beep until the answer has played, and three
quick beeps tell you the server stopped listening. **The firmware on the robot
today is this file with the sample rate back at 16 kHz and the volume gain at
3.0**; it lives in `firmware/esp32_voice_assistant/`. `ESP32_Speaker_Test_Beep.ino`
(Apr 5) is a fresh stand-alone speaker test extracted from the same beep code.

### The server

Only the final server survived. Clues to its past: the project's root namespace
is still `RelayServerDebug`, a `relay-debug.log` shows three launches of a relay
server at 2 AM on Mar 30, and 49 firmware files talk about "the PC". The
unified server (Apr 7) took over the PC's job. Its comments still said "8 kHz"
while it ran at 16 kHz; those comments are fixed in the current copy, and a
scrubbed copy of the exact April file is in `archive/server-original/`.

## Lessons the versions teach

- **Never block inside the WebSocket callback.** Most of the February drop-outs
  were a 2 s wait inside the callback against a 3 s heartbeat.
- **Stream, do not buffer.** Every "buffer the whole answer" attempt died on the
  ESP32's fragmented heap; per-chunk playback with a small queue always worked.
- **Cross-core flags must be `volatile`.** The "mic not recording" bug of Feb 22.
- **Set `mck_io_num` after building the pin struct.** Designated initialisers in
  the wrong order do not compile with the ESP32 toolchain.
- **Use `use_apll = false`** on the microphone port to avoid the MCLK error.
- **Power banks sag.** Lower speaker gain or disable the brown-out detector.
- **Mute the mic while the robot talks or beeps**, or the robot hears itself.
