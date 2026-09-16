# Robot server (C# / .NET 8)

A single-file console program that turns the ESP32's microphone stream into a
spoken answer. See [`../docs/protocol.md`](../docs/protocol.md) for the messages
and [`Program.cs`](Program.cs) for the code (it is ~900 lines and heavily
commented).

## What it needs

| Requirement | Why |
|---|---|
| .NET 8 SDK (or newer; change `TargetFramework` in `server.csproj` if you only have 9/10) | build and run |
| `ffmpeg` on the `PATH` | converts the OpenAI TTS mp3 into raw 8 kHz PCM for the ESP32 |
| **Your own** OpenAI API key, created at https://platform.openai.com/api-keys | Whisper (speech-to-text), gpt-4o-mini (answers), tts-1 (speech); usage is billed to your OpenAI account, roughly a few cents per question |
| Port 8080 reachable from the robot | plain `ws://`, so use it on a LAN or a VPS you trust |

## Configuration (environment variables)

| Variable | Required | Default | Meaning |
|---|---|---|---|
| `OPENAI_API_KEY` | **yes** | - | The server refuses to start without it. |
| `ROBOT_AUTH_TOKEN` | strongly recommended on a public VPS | empty = accept anyone | Shared secret the ESP32 sends when it registers (`ROBOT_AUTH_TOKEN` in the firmware's `secrets.h`). Without it, anyone who finds the port can burn your OpenAI credit. **Only the experimental firmware sends it**; with the main firmware leave it unset and firewall the port instead. |
| `ROBOT_PORT` | no | `8080` | WebSocket port. |
| `ROBOT_SAMPLE_RATE` | no | `8000` | Audio rate in Hz, must equal `AUDIO_SAMPLE_RATE` in the firmware. 8000 streams smoothly on a hotspot; 16000 sounds clearer but may stutter. |
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
two one-time commands are needed in a PowerShell started *as Administrator*: the
first lets a normal program use the port (the server prints it if it is missing),
the second lets the robot reach the PC (Windows shows no firewall pop-up for this
kind of server):

```powershell
netsh http add urlacl url=http://+:8080/ user=Everyone
netsh advfirewall firewall add rule name="Robot server 8080" dir=in action=allow protocol=TCP localport=8080
```

## Deploy on a Linux VPS

`dotnet run` (above) compiles on every start and stops when you log out. On a
VPS the server is compiled once with `dotnet publish` and run by systemd, which
starts it at boot and restarts it if it crashes. Tested layout: Ubuntu 22.04 or
24.04; any small VPS works (the original ran on a cheap Hostinger KVM).

```bash
# 1. tools (Debian and other distributions: add Microsoft's package feed first,
#    see https://learn.microsoft.com/dotnet/core/install/linux, then the same apt line)
sudo apt update && sudo apt install -y ffmpeg git dotnet-sdk-8.0
dotnet --version                                    # must print a version

# 2. code
git clone https://github.com/mohtheghost/cardboard-robot-voice-assistant.git ~/robot
cd ~/robot/server

# 3. an unprivileged user and the install folder
sudo useradd -r -s /usr/sbin/nologin -d /opt/robot-server robot
sudo mkdir -p /opt/robot-server && sudo chown "$USER" /opt/robot-server
dotnet publish -c Release -o /opt/robot-server
sudo chown -R robot:robot /opt/robot-server

# 4. secrets: root-only file, one KEY=value per line, no quotes
sudo install -m 600 /dev/null /etc/robot-server.env
sudo nano /etc/robot-server.env                      # OPENAI_API_KEY=sk-...   (+ ROBOT_AUTH_TOKEN=... only with the experimental firmware)

# 5. service
sudo cp deploy/robot-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now robot-server
journalctl -u robot-server -f                        # live log; Ctrl+C to leave it
```

After changing the server code: `cd ~/robot && git pull`, then repeat the
`dotnet publish` and `chown` lines and `sudo systemctl restart robot-server`.
After changing `/etc/robot-server.env`: `sudo systemctl restart robot-server`.

**Open the port, but not to everyone.** Two firewalls are involved: the
provider's own firewall (a "security group" or "cloud firewall" in the provider's
web panel) must allow inbound TCP 8080, and `ufw` on the machine must too. Pick
the rule that matches your robot:

- Robot on your home WiFi with the **tested firmware** (sends no token): find your
  home's public address from a PC at home (`curl -4 ifconfig.me`) and allow only
  it:

  ```bash
  sudo ufw allow OpenSSH                                       # first, or you lock yourself out
  sudo ufw allow from <your-home-ip> to any port 8080 proto tcp
  sudo ufw enable && sudo ufw status
  ```

  Home addresses change now and then; if the robot stops connecting, check the
  address again. Leave `ROBOT_AUTH_TOKEN` out of the env file: the tested
  firmware would be rejected.
- Robot on a phone hotspot or anywhere else (its address changes constantly):
  set `ROBOT_AUTH_TOKEN` in the env file and in the robot's `secrets.h`, flash
  `firmware/experimental/esp32_voice_assistant_next` (the only main firmware that
  sends the token), and open the port to everyone: `sudo ufw allow OpenSSH &&
  sudo ufw allow 8080/tcp && sudo ufw enable`.

Check from home before touching the robot: Windows
`Test-NetConnection <vps-ip> -Port 8080` (TcpTestSucceeded : True), Linux/macOS
`nc -zv <vps-ip> 8080`. If that fails while `sudo ss -ltnp | grep 8080` on the
VPS shows the server listening, a firewall is in the way.

The provided [`deploy/robot-server.service`](deploy/robot-server.service) runs the
server as the unprivileged `robot` user in `/opt/robot-server`, restarts it if it
crashes, and reads the secrets from `/etc/robot-server.env` (root-only, mode 600).

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
