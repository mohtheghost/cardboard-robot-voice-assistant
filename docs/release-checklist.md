# Before you show it to the world

What was missing or dangerous in the original folder, what has been fixed
during the clean-up, and what only you can do. Work through the **Must do**
list before the first `git push`.

## Must do (only you can)

1. **Rotate the OpenAI API key.** The old key was written as a constant in
   `server/Program.cs`, is compiled into `server/bin/**/server.dll` and
   `server/obj/**/server.dll`, and is indexed by Visual Studio in `server/.vs/`.
   Treat it as leaked: create a new key at platform.openai.com and delete the old
   one. The new server reads `OPENAI_API_KEY` from the environment.
2. **Delete the build folders that still contain the old key and your test
   recordings** (they are git-ignored, but a zip or a copy would include them):

   ```powershell
   Remove-Item -Recurse -Force server\bin, server\obj, server\.vs, experiments\whisper-test\bin, experiments\whisper-test\obj, experiments\whisper-test\.vs
   ```

   `experiments/whisper-test/bin` also holds `audio.wav`, `output.wav`,
   `audio.pcm` and `tts_output.mp3`, recordings of your own voice. Copy them
   somewhere first if you want to keep them.
3. **Change your home WiFi password.** It sat in 58 firmware files that were
   downloaded from and possibly uploaded to an AI chat. The files in `archive/`
   now contain `YOUR_WIFI_PASSWORD` instead, and `secrets.h` is git-ignored.
4. **Put your name in `LICENSE`** (replace `<YOUR NAME>`), or pick another
   license.
5. **Protect the server before you tell anyone its address.** Either set
   `ROBOT_AUTH_TOKEN` on the server *and* flash the experimental firmware that
   sends it, or restrict port 8080 in the VPS firewall to your home or hotspot
   IP. Without one of these, anyone who finds the port can spend your OpenAI
   credit. The server prints a warning at startup while the token is unset.
6. **Keep `media/` out of the repository.** The original photos carry GPS EXIF
   data and the two `.MOV` files contain the recording location; together they
   are about 240 MB. `.gitignore` excludes `media/`; the re-encoded copies in
   `docs/images` and `docs/media` have no metadata.
7. **Create the repository:**

   ```bash
   git init
   git add .
   git status        # check that no bin/, obj/, .vs/, media/ or secrets.h is listed
   git commit -m "Cardboard robot voice assistant"
   ```

## What was missing and is now added

| Gap | Now |
|---|---|
| No README, no docs, no license | `README.md`, `docs/hardware.md`, `docs/protocol.md`, `docs/history.md`, `LICENSE` (MIT) |
| 61 firmware files in one folder, no way to know which one runs on the robot | `firmware/esp32_voice_assistant/` is the running code; everything else is in `archive/firmware-versions/` with an index |
| The current firmware was not in the folder at all (only in a chat paste) | Saved as the main firmware |
| WiFi name, password and VPS IP in every firmware file | `secrets.h` (git-ignored) + `secrets.h.example` |
| OpenAI key hard-coded in the server | `OPENAI_API_KEY` environment variable; the server refuses to start without it |
| No authentication on the server | Optional `ROBOT_AUTH_TOKEN` (needs the experimental firmware) |
| No way to run the server as a service | `server/deploy/robot-server.service` + `server/README.md` |
| Hardware tests scattered among prototypes | `firmware/hardware_tests/` (speaker beep, microphone stream) |
| 240 MB of originals with location metadata | `media/` (ignored) + stripped, web-sized copies in `docs/` |
| Stale "8 kHz" comments everywhere | Fixed in the server and the experimental firmware; documented in the main firmware header |
| Build outputs (`bin/`, `obj/`, `.vs/`) with secrets inside | `.gitignore` + item 2 above |

## Code fixes applied to the server (all compile-verified, defaults unchanged)

- `OPENAI_API_KEY`, `ROBOT_PORT`, `ROBOT_WAKE_WORDS`, `ROBOT_LANGUAGE`,
  `ROBOT_TTS_VOICE`, `ROBOT_AUTH_TOKEN`, `ROBOT_KEEP_RECORDINGS` environment
  variables (same defaults as the April code).
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
`firmware/experimental/esp32_voice_assistant_next/`, which still needs a test
on the real robot before it can replace the main firmware.

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
- The 8 kHz strings in logs and banners are stale (the rate is 16 kHz).

## Nice to have later

- Local wake-word detection on the ESP32 (ESP-SR / Porcupine) so audio is only
  sent after the wake word: far cheaper and more private than transcribing every
  sentence.
- TLS: put Caddy or nginx in front of the server and use `wss://` from the
  firmware (`beginSSL`).
- Conversation memory (send the last few exchanges to ChatGPT).
- Binary WebSocket frames instead of base64 JSON (33 % less bandwidth).
- A short build log or video of the cardboard body for the README.
