# Robot server (C# / .NET 8)

A single-file console program that turns the ESP32's microphone stream into a
spoken answer. See [`../docs/protocol.md`](../docs/protocol.md) for the messages
and [`Program.cs`](Program.cs) for the code (it is ~900 lines and heavily
commented).

## What it needs

| Requirement | Why |
|---|---|
| .NET 8 SDK (or newer; change `TargetFramework` in `server.csproj` if you only have 9/10) | build and run |
| `ffmpeg` on the `PATH` | converts the OpenAI TTS mp3 into raw 16 kHz PCM for the ESP32 |
| **Your own** OpenAI API key, created at https://platform.openai.com/api-keys | Whisper (speech-to-text), gpt-4o-mini (answers), tts-1 (speech); usage is billed to your OpenAI account, roughly a few cents per question |
| Port 8080 reachable from the robot | plain `ws://`, so use it on a LAN or a VPS you trust |

## Configuration (environment variables)

| Variable | Required | Default | Meaning |
|---|---|---|---|
| `OPENAI_API_KEY` | **yes** | - | The server refuses to start without it. |
| `ROBOT_AUTH_TOKEN` | strongly recommended on a public VPS | empty = accept anyone | Shared secret the ESP32 sends when it registers (`ROBOT_AUTH_TOKEN` in the firmware's `secrets.h`). Without it, anyone who finds the port can burn your OpenAI credit. **Only the experimental firmware sends it**; with the main firmware leave it unset and firewall the port instead. |
| `ROBOT_PORT` | no | `8080` | WebSocket port. |
| `ROBOT_BIND` | no | `+` (all interfaces) | Set to `localhost` for a test on your own PC only (no admin rights needed on Windows). The robot can only reach the server when this is `+`. |
| `ROBOT_WAKE_WORDS` | no | `assistant,hello,robot` | Comma-separated. The transcribed sentence must contain one of them, the word is then removed and the rest is sent to ChatGPT. |
| `ROBOT_LANGUAGE` | no | `en` | Whisper language hint (`ar` for Arabic, etc.). Also update the system prompt in `GetChatResponseAsync` if you change language. |
| `ROBOT_TTS_VOICE` | no | `echo` | One of `alloy, echo, fable, onyx, nova, shimmer`. |
| `ROBOT_KEEP_RECORDINGS` | no | `0` | Set to `1` to keep every `recording_*.wav` on disk (useful for tuning the VAD). |

## Run it on your PC (quick test)

```powershell
# Windows PowerShell
$env:OPENAI_API_KEY = "sk-..."
$env:ROBOT_AUTH_TOKEN = "pick-a-long-random-string"
cd server
dotnet run
```

```bash
# Linux / macOS
export OPENAI_API_KEY=sk-...
export ROBOT_AUTH_TOKEN=pick-a-long-random-string
cd server
dotnet run
```

These variables last for the current terminal only. To make them permanent use
`setx OPENAI_API_KEY "sk-..."` on Windows (then reopen the terminal) or add the
`export` lines to `~/.bashrc` on Linux/macOS.

Then put your PC's LAN IP in the firmware's `secrets.h` (`VPS_HOST`). On Windows
listening on all interfaces needs the URL reserved once (run as Administrator;
the server prints this command if it is missing):

```powershell
netsh http add urlacl url=http://+:8080/ user=Everyone
```

and the Windows firewall must allow inbound TCP 8080.

## Deploy on a Linux VPS

```bash
sudo apt install -y ffmpeg dotnet-sdk-8.0        # Ubuntu 22.04+/Debian 12: see Microsoft's install page if the package is missing
git clone <your repo> ~/robot && cd ~/robot/server
dotnet publish -c Release -o /opt/robot-server
sudo cp deploy/robot-server.service /etc/systemd/system/
sudo nano /etc/systemd/system/robot-server.service   # put your key and token in the Environment= lines
sudo systemctl daemon-reload
sudo systemctl enable --now robot-server
sudo ufw allow 8080/tcp
journalctl -u robot-server -f                         # live log
```

The provided [`deploy/robot-server.service`](deploy/robot-server.service) runs the
server as an unprivileged user in `/opt/robot-server`, restarts it if it crashes,
and reads the secrets from the unit file (keep it `chmod 600`).

## What you will see in the console

```
[VAD] 🎯 Dynamic VAD - calibrating background noise for 3s...
[VAD] ✅ Calibration complete!   Background noise: 210.3   Speech threshold: 1210.3
[VAD] ⚪ silence | RMS:  180.1 | Threshold: 1210.3 | ████|
[VAD] 🎙️  SPEECH STARTED
[VAD] 🔇 SPEECH ENDED (1840ms)
[STT] ✅ Transcription: Hello robot, tell me a joke.
[WAKE] 🚨 Wake word detected: hello
[ChatGPT] ✅ Response: Why did the robot go on vacation? It needed to recharge!
[TTS] ✅ Generated 41KB MP3
[SEND] 🚀 Sending 125KB audio in 63 chunks
```

Keep quiet for the first 3 seconds after the robot connects: that is when the
background-noise level is measured.

## Files it writes (next to the executable)

- `recording_YYYYMMDD_HHMMSS.wav` while a sentence is being recorded; deleted after
  transcription unless `ROBOT_KEEP_RECORDINGS=1`.
- `transcriptions/<same name>.txt` with the text Whisper returned (small, kept).
- `temp_tts_*.mp3 / .pcm` for a second during TTS conversion.

## Cost note

Every sentence the VAD detects is sent to Whisper, **even without a wake word**
(the wake word is checked on the transcription). In a noisy room that can add up,
so raise `VadNoiseOffset` in `Config`, use the auth token, and stop the service
when the robot is not in use.
