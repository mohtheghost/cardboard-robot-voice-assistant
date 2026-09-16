# Release notes and checklist

What was missing when this project was tidied up for publication, what was
changed, and what is still known to be rough. Also a short checklist for anyone
who forks it and wants to publish their own copy.

## Before publishing your own copy

1. **Never commit a key.** The server reads `OPENAI_API_KEY` from the
   environment and refuses to start with a placeholder. If a real key ever
   ended up in a file or a build folder, revoke it at platform.openai.com and
   create a new one.
2. **Delete build folders** (`bin/`, `obj/`, `.vs/`) before zipping or sharing
   the tree any other way; Git already ignores them.
3. **Keep `secrets.h` and full-size media out of Git.** Both are ignored; the
   photos in `docs/images` are re-encoded copies without EXIF or GPS data.
4. **Protect the server** before telling anyone its address: set
   `ROBOT_AUTH_TOKEN` (requires the experimental firmware) or restrict the port in
   the VPS firewall to your own IP.
5. **Check `git status` before pushing**: no `secrets.h`, `bin/`, `obj/`,
   `.vs/` or `media/` should be listed.
6. Put the name you want on the MIT notice in `LICENSE`.

## What was missing and is now added

| Gap | Now |
|---|---|
| No README, no docs, no license | `README.md`, `docs/hardware.md`, `docs/protocol.md`, `docs/history.md`, `LICENSE` (MIT) |
| 61 firmware files in one folder, no way to know which one runs on the robot | `firmware/esp32_voice_assistant/` is the running code; everything else is in `archive/firmware-versions/` with an index |
| The current firmware existed only in a chat paste | Saved as the main firmware |
| WiFi name, password and server IP in every firmware file | `secrets.h` (git-ignored) + `secrets.h.example` |
| OpenAI key hard-coded in the server | `OPENAI_API_KEY` environment variable; the server refuses to start without a real-looking key |
| No authentication on the server | Optional `ROBOT_AUTH_TOKEN` (needs the experimental firmware) |
| No way to run the server as a service | `server/deploy/robot-server.service` + `server/README.md` |
| Hardware tests scattered among prototypes | `firmware/hardware_tests/` (speaker beep, microphone stream) |
| 240 MB of originals with location metadata | `media/` (ignored) + stripped, web-sized copies in `docs/` |
| Sample rate mismatched between comments and code | 8 kHz on both sides again (the configuration that streamed without stutter); server rate configurable with `ROBOT_SAMPLE_RATE` |
| Build outputs (`bin/`, `obj/`, `.vs/`) with secrets inside | `.gitignore` |

## Code fixes applied to the server (compile-verified, defaults unchanged)

- `OPENAI_API_KEY`, `ROBOT_PORT`, `ROBOT_WAKE_WORDS`, `ROBOT_LANGUAGE`,
  `ROBOT_TTS_VOICE`, `ROBOT_AUTH_TOKEN`, `ROBOT_KEEP_RECORDINGS` environment
  variables (same defaults as the April code).
- Clear messages when the key is missing, a placeholder, rejected (HTTP 401) or
  out of credit (HTTP 429).
- One WebSocket send at a time (`.NET` throws if two `SendAsync` calls overlap;
  the "speech ended" beep and a running answer could collide).
- Audio is ignored for 0.7 s after telling the robot to beep, so the robot's own
  beeps are no longer transcribed by Whisper (they used to trigger a fake
  "utterance" and a paid API call each time).
- `{"type":"listen"}` is sent when a wake word was heard but no answer follows,
  so the experimental firmware can un-mute its microphone.
- Recordings are deleted after transcription unless `ROBOT_KEEP_RECORDINGS=1`
  (the disk used to fill up with every sentence ever overheard).
- Previous robot connection is closed when the robot reconnects; the audio
  processor is disposed on disconnect (it used to leak a task per reconnect).
- `ffmpeg` is probed at startup with a clear error, runs quietly, and its output
  is read while it runs (a full pipe could hang the old code).
- Incoming messages are capped at 64 KB.

## Known weak spots that were NOT changed in the working firmware

These are documented, not fixed, in `firmware/esp32_voice_assistant/` because
that file is the tested one. All of them are addressed in
`firmware/experimental/esp32_voice_assistant_next/`, which compiles cleanly but
still needs a test on the real robot before it can replace the main firmware.

- If the wake word is heard but the server never sends an answer (empty command,
  API error), the microphone stays muted until the robot is power-cycled.
- Waiting for queue space calls `webSocket.loop()` from inside the WebSocket
  callback; a long answer can make the callback re-enter itself and overwrite
  the shared base64 buffer.
- The last ~130 ms of every answer is cut off (`i2s_zero_dma_buffer` runs
  before the DMA ring has played out).
- The robot's beeps are heard by its own microphone (now handled on the server
  side by the 0.7 s ignore window).
- The periodic test beep (`BEEP_ENABLED true`) is on and can slip in between two
  answer chunks.
- A WiFi timeout at boot halts the board forever instead of retrying.
- `SPK_VOLUME_GAIN 3.0` clips loud parts of the answer; lowering it and raising
  the amplifier's GAIN pin would sound cleaner.

## Not yet re-tested with a real key

The server was refactored for publication and smoke-tested with a simulated
robot (registration, voice detection, beep messages, error handling), but the
OpenAI calls and the complete question-to-answer chain have not been re-run with
a real API key on the physical robot since the refactor. Do that first when you
pick the project up again; the April server is in `archive/server-original/` if
anything needs comparing.

## Nice to have later

- Local wake-word detection on the ESP32 (ESP-SR / Porcupine) so audio is only
  sent after the wake word: far cheaper and more private than transcribing every
  sentence.
- TLS: put Caddy or nginx in front of the server and use `wss://` from the
  firmware (`beginSSL`).
- Conversation memory (send the last few exchanges to ChatGPT).
- Binary WebSocket frames instead of base64 JSON (33 % less bandwidth).
- A short build log or video of the cardboard body for the README.
