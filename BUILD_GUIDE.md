# Build your own cardboard robot voice assistant

A step-by-step guide for someone who has never used an ESP32, PowerShell or .NET.
You do not need to understand the code. If you can follow a recipe, you can build
this. Plan for one weekend: an afternoon for the electronics and software, an
afternoon for the body.

**What you end up with:** a robot that listens when you say "hello robot ...",
beeps to show it heard you, asks ChatGPT, and answers out loud through its own
speaker.

**What it costs:** the electronics are cheap (the ESP32 board, the microphone
module, the amplifier module and a small speaker cost a few dollars each). Talking
to the robot costs a few cents per question at most, on your own OpenAI account.

The guide is written for Windows; Linux and macOS commands are given where they
differ.

---

## 1. Shopping list

| Part | What to search for | Notes |
|---|---|---|
| ESP32 development board | "ESP32 DevKit V1 30 pin" (also sold as "DOIT ESP32 DevKit") | Micro-USB. Must be a **classic ESP32**, not an ESP32-S3, C3 or S2: those have different pins. The 30-pin DevKit V1 is the safe choice. |
| Microphone | "INMP441 I2S microphone module" | Small round or square board with 6 pins. |
| Amplifier | "MAX98357A I2S amplifier module" | Has a green screw terminal for the speaker. |
| Speaker | "3 W 4 ohm speaker" or "8 ohm mini speaker" | Small enough to fit in a box, with two bare wires (cut the plug off if it has one). |
| Power | Any USB power bank rated 1 A or more + micro-USB cable | Average draw is under 0.5 A, but loud answers pull short peaks above that. |
| Wires | "Dupont jumper wires female to female" | About 12 needed. |
| Tools | A small flat screwdriver (about 2 mm) for the amplifier's screw terminal; scissors or a wire stripper for the speaker wires | Optional: a soldering iron (see below). |
| Body | Cardboard boxes (a tissue box, small shipping boxes), coloured paper, tape, glue, two toothpicks | The toothpicks are the antennae. |

**About soldering.** Most microphone and amplifier modules arrive with the pin
headers loose in the bag, and they have to be soldered on. If you cannot solder,
ask the seller for "headers pre-soldered" or ask a friend; it is ten minutes of
work. While the header is being soldered, also solder a short wire between the
microphone module's `L/R` pad and its `GND` pad (see section 3 for why).

## 2. Software to install on your computer

Do this while you wait for the parts.

1. **Arduino IDE 2** from https://www.arduino.cc/en/software (this flashes the
   robot). Install it and open it once.
2. Inside the Arduino IDE:
   - *File > Preferences*: in *Additional boards manager URLs* paste
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json` and
     click OK.
   - *Tools > Board > Boards Manager*: search `esp32`, install **esp32 by
     Espressif Systems** (a large download, be patient).
   - *Tools > Manage Libraries*: search `WebSockets`, install **WebSockets by
     Markus Sattler**.
3. **.NET 8 SDK** (this runs the server): open
   https://dotnet.microsoft.com/download/dotnet/8.0 and download the **SDK**
   (not the "Runtime"), Windows x64 installer. The front page of the .NET site
   offers a newer version; the newer SDK also works, but 8 is the one this
   project was written for. On Ubuntu: `sudo apt install -y dotnet-sdk-8.0`.
   After installing, open a **new** terminal and run `dotnet --version`; it
   must print a version number.
4. **ffmpeg**: press Start, type `PowerShell`, open it and run
   `winget install ffmpeg`. If it asks whether you agree to the source
   agreement, type `Y` and Enter. When it finishes, **close that window, open a
   new one** and run `ffmpeg -version`; the first line must start with
   `ffmpeg version`. (Programs installed a moment ago are only visible to
   windows opened afterwards.) On Ubuntu: `sudo apt install ffmpeg`; on macOS:
   `brew install ffmpeg`.
5. **An OpenAI API key** (the robot's brain runs on OpenAI, billed to you):
   create an account at https://platform.openai.com, add a small amount of
   credit under *Billing*, then create a key at
   https://platform.openai.com/api-keys. Copy it somewhere safe; it is shown
   only once. **Never paste the key into the code or into any public place.**
6. **This repository**: on the GitHub page click the green *Code* button, then
   *Download ZIP*. Right-click the downloaded ZIP > *Extract All...* and choose a
   simple place such as your *Documents* folder. You get a folder named
   `cardboard-robot-voice-assistant-main`; the rest of this guide calls it "the
   repository". The `server` and `firmware` folders are inside it.
7. **USB driver check**: plug the ESP32 board into the computer. Press Start,
   type `Device Manager`, open it and expand *Ports (COM & LPT)*. You should see
   an entry such as *Silicon Labs CP210x USB to UART Bridge (COM5)* or
   *USB-SERIAL CH340 (COM5)*. If nothing appears, read the marking on the small
   square chip next to the board's USB socket: `CP2102` needs the driver from
   https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers,
   `CH340` needs the driver from https://www.wch-ic.com/downloads/CH341SER_EXE.html.
   Install it, unplug and re-plug the board. If still nothing, try another USB
   cable: many are charge-only.

## 3. Wire the electronics

Do this on the table first, before any cardboard. Unplug the USB cable while
wiring. The pin names are printed on the ESP32 board next to each pin.

### Amplifier and speaker

| MAX98357A pin | goes to ESP32 pin |
|---|---|
| VIN | VIN (5 V, the pin next to a GND at the USB end of the board) |
| GND | the GND pin next to VIN |
| BCLK | D12 |
| LRC | D14 |
| DIN | D13 |
| GAIN, SD | leave unconnected |

Loosen the two screws of the amplifier's green terminal, push one bare speaker
wire into each hole and tighten. Which wire goes where does not matter.

### Microphone

| INMP441 pin | goes to ESP32 pin |
|---|---|
| VDD | 3V3 |
| GND | the GND pin next to 3V3 |
| SCK | D25 |
| WS | D27 |
| SD | D18 |
| L/R | must be connected to GND (see below) |

**The GND problem.** The 30-pin board has only two GND pins, and you need three
ground connections (amplifier GND, microphone GND, microphone L/R). Solution: on
the microphone module the `L/R` pin sits right next to its own `GND` pin. Join
those two on the module itself: solder a short wire across the two pads when the
header is soldered, or twist a short bare wire around both header pins. Then only
two GND jumpers go to the ESP32, as in the tables. If you would rather not
solder, buy a small breadboard and a few male-to-female jumpers and put all three
grounds on one breadboard row.

Before plugging in, check: the three ground connections, the microphone on
**3V3** (not 5 V), the amplifier on **VIN**. Photos of the real wiring are in
[`docs/hardware.md`](docs/hardware.md).

## 4. Test the speaker (10 minutes)

1. Open the Arduino IDE. *File > Open* and pick
   `firmware/hardware_tests/speaker_beep_test/speaker_beep_test.ino` inside the
   repository.
2. *Tools > Board > esp32 > DOIT ESP32 DEVKIT V1*. The list is long and
   alphabetical; typing `doit` in the *Select Board* box at the top of the
   window is faster.
3. Find the port: open *Tools > Port* with the board **unplugged** and note what
   is listed (often only `COM1`). Plug the board in, wait five seconds, open
   *Tools > Port* again: the new entry, for example `COM5`, is the board. Choose
   it.
4. Click the **Upload** arrow (top left). The first upload compiles for a minute
   or two and the black output panel scrolls a lot. Orange lines mentioning
   "deprecated" or "i2s" are warnings and are fine. The upload has worked when
   the last lines say `Hash of data verified` and `Hard resetting via RTS pin...`.
5. You should hear a short beep every half second.

If the output ends with `A fatal error occurred: Failed to connect to ESP32: No
serial data received`, click Upload again and hold the **BOOT** button on the
board from the moment `Connecting....` appears until the dots stop.

If the upload worked but there is no sound: check BCLK/LRC/DIN, check that VIN
is really on the 5 V pin, and check the speaker wires in the screw terminal.

## 5. Start the server on your computer (15 minutes)

The server is the robot's brain. For the first tests run it on the same computer
and the same WiFi as the robot.

1. **Open the server window.** In File Explorer open the repository folder, then
   the `server` folder inside it. Right-click an empty white area of the folder
   and choose *Open in Terminal*. A window opens whose last line begins with
   `PS C:\...\server>`. This window is "the server window" for the rest of the
   guide; keep it open. (If the line does not start with `PS`, type
   `powershell` and press Enter.)
2. **Windows only, once.** Press Start, type `PowerShell`, right-click *Windows
   PowerShell*, choose *Run as administrator* and click *Yes*. In that window run
   these two lines, then close it:

   ```powershell
   netsh http add urlacl url=http://+:8080/ user=Everyone
   netsh advfirewall firewall add rule name="Robot server 8080" dir=in action=allow protocol=TCP localport=8080
   ```

   The first line lets a normal program use port 8080 (without it the server
   stops with *Access is denied*); the second lets the robot reach your PC
   (Windows shows no pop-up for this kind of server, so the rule has to be added
   by hand). Close the administrator window afterwards and never start the
   server from it.
3. **Give the server your key.** Back in the server window (the `PS ...\server>`
   one), type, with your own key between the quotes:

   ```powershell
   $env:OPENAI_API_KEY = "sk-paste-your-key-here"
   ```

   On Linux/macOS: `export OPENAI_API_KEY=sk-paste-your-key-here`. This lasts
   only for this window: if you ever open a new one, type it again.
4. **Start it:**

   ```powershell
   dotnet run
   ```

   The first start compiles for about a minute (text about "Welcome to .NET" or
   "Restore" is normal). Then you should see the banner, several `✅` lines, a
   yellow `⚠️ ROBOT_AUTH_TOKEN is not set` line (expected on your home network;
   it matters only in section 10), and finally:

   ```
   🚀 Server listening on http://+:8080/
   📡 Waiting for ESP32 to connect...
   ```

   Leave the window open. To stop the server later, click in the window and
   press Ctrl+C; to start it again type `dotnet run`.
5. **Find your computer's address on the WiFi.** In the server window (the
   server can keep running; open a second terminal the same way if you prefer)
   run `ipconfig`. Scroll to the block headed *Wireless LAN adapter Wi-Fi* (or
   *Ethernet adapter Ethernet* if the PC uses a cable) and take the *IPv4
   Address* from that block, usually `192.168.x.x`. Ignore blocks named
   vEthernet, WSL, VPN, VirtualBox or Bluetooth. On Linux: `hostname -I` (first
   address); on macOS: `ipconfig getifaddr en0`. Write the address down; the
   robot needs it. Routers sometimes give the PC a different address later; if
   the robot stops connecting one day, check it again.

## 6. Test the microphone (15 minutes)

1. In the Arduino IDE open
   `firmware/hardware_tests/mic_stream_test/mic_stream_test.ino`.
2. **Create `secrets.h`.** In File Explorer open the same folder
   (`firmware/hardware_tests/mic_stream_test`). Right-click
   `secrets.h.example` > *Open with* > *Notepad*, press Ctrl+A then Ctrl+C.
   (Explorer may show the file as just `secrets.h` because it hides the
   `.example` part; turn on *View > Show > File name extensions* to see real
   names.) Back in the Arduino IDE click the `...` button at the top right of
   the editor, choose *New Tab*, type exactly `secrets.h`, press OK, then Ctrl+V
   and Ctrl+S. You now have a `secrets.h` tab next to `mic_stream_test.ino`.
3. Fill it in. Keep the quotes. A finished file looks like this:

   ```c
   #define WIFI_SSID        "MyHomeWifi"        // 2.4 GHz network only
   #define WIFI_PASS        "my-wifi-password"
   #define VPS_HOST         "192.168.1.23"      // your computer's address from section 5
   #define VPS_PORT         8080                // no quotes here
   #define ROBOT_AUTH_TOKEN ""                  // leave empty for now
   ```

   The ESP32 cannot see 5 GHz networks; if your router has one name for both,
   most routers still let the ESP32 join the 2.4 GHz side.
4. Click Upload. Then open *Tools > Serial Monitor* and set the speed box at
   the right to **115200 baud**. If the monitor is empty, press the small `EN`
   button on the board to restart it and watch from the beginning. You should
   see the WiFi connect and `MICROPHONE NOW STREAMING`. If you only see a
   growing line of dots for more than a minute, the WiFi name or password is
   wrong or the network is 5 GHz: this test never gives up on its own. Fix
   `secrets.h` and upload again.
5. Now look at the **server window** and **stay silent for three seconds**: the
   server measures the background noise then and prints
   `[VAD] ✅ Calibration complete!`. After that it prints a line like
   `[VAD] ⚪ silence | RMS: 180.1 | Threshold: 1210.3 | ████|` every two seconds.
   Say a sentence near the microphone: the bar must grow past the `|` mark and
   `[VAD] 🎙️ SPEECH STARTED` must appear, and about two seconds after you stop,
   `SPEECH ENDED`. The server then also prints a `[STT] ✅ Transcription:` line
   (your words, recognised by Whisper) and `[WAKE] No wake word detected,
   ignoring`. That is normal: the real pipeline is already running, nothing plays
   back yet, and each sentence costs a fraction of a cent.

If the bar never moves when you speak, re-check the microphone wiring: SCK, WS,
SD, and L/R connected to GND.

## 7. Flash the real firmware and talk to it

1. Close the mic-test window first (*File > Close*). Open
   `firmware/esp32_voice_assistant/esp32_voice_assistant.ino` and create
   `secrets.h` in that folder exactly as in section 6, with the same values.
2. Click Upload. If it ends with `could not open port 'COMx'`, close the Serial
   Monitor in every other IDE window and try again.
3. Open the Serial Monitor (115200). You should see `ESP32 REGISTERED`. The blue
   LED on the board turns **off** once the robot is connected to the server, and
   you will hear a very faint tick every two seconds: that is the built-in
   speaker test, it is normal (section 8 shows how to turn it off).
4. **Stay quiet for three seconds** after it connects (background noise is
   measured again).
5. Say **"Hello robot, tell me a joke."** and then stay quiet. About two seconds
   after you stop talking you hear **three quick beeps**: the server noticed the
   end of your sentence (do not repeat it). A few seconds later **one longer
   beep**: the wake word was recognised and the robot is thinking. Five to ten
   seconds after that the answer plays.

The wake words are `hello`, `robot` and `assistant`. One of them must appear
somewhere in the sentence; the server deletes that word and sends the rest of
the sentence to ChatGPT, so "tell me a joke, robot" works too. If you say only
the wake word and nothing else, the tested firmware stays muted (see section 11).

## 8. Tune it for your room

Firmware values are near the top of
`firmware/esp32_voice_assistant/esp32_voice_assistant.ino`; after changing one,
click Upload again. Server values are in `server/Program.cs` (open it with
Notepad); after changing one, click in the server window, press Ctrl+C and run
`dotnet run` again (it recompiles).

| Problem | Change |
|---|---|
| It reacts to nothing, or to everything | Server, the line `public const double VadNoiseOffset = 1000.0;`: raise it (for example 1500) if it triggers by itself, lower it (for example 600) if you have to shout. |
| Answer is distorted or crackly | Firmware `SPK_VOLUME_GAIN` (default 3.0): try 2.0 or 1.5. |
| Answer is too quiet | The software gain is already at its useful maximum. Connect the amplifier's GAIN pin to GND for a bit more, or use a 4 ohm speaker. |
| Microphone too quiet or clipping | Firmware `MIC_GAIN_MULTIPLIER` (default 1.5). |
| Other wake words or voice | In the server window, before `dotnet run`: `$env:ROBOT_WAKE_WORDS = "hello,robot,computer"` or `$env:ROBOT_TTS_VOICE = "nova"` (values in `server/README.md`). |
| Another language | `$env:ROBOT_LANGUAGE = "ar"` (or `fr`, `de`...) makes Whisper expect that language. To get *answers* in that language, also change the sentence `Always respond in English` in `server/Program.cs`. |
| The faint tick every 2 seconds annoys you | Firmware `BEEP_ENABLED true` -> `false`. |

## 9. Build the body

Look at the photos in [`docs/hardware.md`](docs/hardware.md) for the layout that
worked:

- **Chest**: the biggest box. The ESP32, the amplifier and the wires live here.
  Tape the boards to the inside wall so nothing rattles.
- **Speaker**: cut a hole in the back or front panel and tape the speaker behind
  it. Sound needs a hole; cardboard muffles it a lot.
- **Microphone**: put it in an arm or the head, as far from the speaker as the
  body allows, behind a small hole. The firmware mutes the microphone while the
  robot talks; distance takes care of the rest (its own beeps and the end of an
  answer) so it does not answer itself.
- **Power**: the power bank can sit inside the chest with the cable coming out
  of the bottom, or outside the robot if it is too heavy.
- **Head, arms, legs**: smaller boxes, glued or taped on. Draw the face, add the
  toothpick antennae, cover everything in coloured paper.
- Leave one panel closed with tape, not glue, so you can reach the USB port for
  re-flashing.

Make all the wires long enough before you close the boxes, and test once more
with the lid open before you decorate.

## 10. Make it work away from your computer (optional)

On your PC you started the server with `dotnet run`: it compiles the code every
time and runs in the foreground of that window, which is right for testing but
stops when the window closes. To use the robot without your PC, put the server on
a small Linux VPS. The original robot's server ran on a cheap **Hostinger KVM
VPS** (Ubuntu) and that is what the recipe was written for, but any provider
with a Linux machine and a public address works. The
recipe in [`server/README.md`](server/README.md), section *Deploy on a Linux
VPS*, compiles the server once with `dotnet publish` and installs it as a
service that starts at boot and restarts itself. Follow it exactly, then:

- Put the VPS address in the robot's `secrets.h` (`VPS_HOST`) and re-flash.
- **Protect the port.** Anyone who finds an open port 8080 can spend your OpenAI
  credit. You have two options, and which one you can use depends on where the
  robot is:
  - *Robot on your home WiFi, tested firmware:* keep `ROBOT_AUTH_TOKEN` **unset**
    on the server (the tested firmware never sends a token and would be rejected)
    and allow port 8080 only from your home connection in the VPS firewall. The
    README recipe shows the `ufw` commands.
  - *Robot on a phone hotspot, or anywhere:* the robot's address changes all the
    time, so the firewall option does not work. Set `ROBOT_AUTH_TOKEN` on the
    server **and** in `secrets.h`, and flash
    `firmware/experimental/esp32_voice_assistant_next` (the tested firmware
    cannot send the token). That firmware compiles but has not yet been verified
    on a real robot: test it with the lid open. It must answer a question, resume
    listening within a second after you say only the wake word, and play a long
    answer ("tell me a story") without gaps. If it misbehaves, re-flash the
    tested firmware, remove the token from the server, and use the firewall
    option on home WiFi.
- **Phone hotspot catches.** Set the hotspot to 2.4 GHz (iPhone: *Personal
  Hotspot > Maximize Compatibility* on; Android: hotspot settings > *AP band*
  2.4 GHz), otherwise the ESP32 never sees it. `secrets.h` holds one network, so
  switching between home WiFi and hotspot means a re-flash, unless you give the
  hotspot the same name and password as your home WiFi. Turn the hotspot on
  **before** powering the robot: the tested firmware gives up on WiFi after
  20 seconds and then stays dead until you press the `EN` button or re-plug the
  power. Phones also switch the hotspot off after a few idle minutes.

## 11. When something does not work

| Symptom | Likely cause and fix |
|---|---|
| No COM port in the IDE or Device Manager | USB driver missing (section 2, step 7), or a charge-only USB cable. Try another cable. |
| `WebSocketsClient.h: No such file or directory` | The WebSockets library is not installed: *Tools > Manage Libraries*. |
| `secrets.h: No such file or directory` | The file was not created in the sketch folder, or is really named `secrets.h.txt`: section 6, step 2. |
| `Failed to connect to ESP32: No serial data received` or `Wrong boot mode detected` | Hold the BOOT button while `Connecting....` is shown. |
| `could not open port 'COMx'` | Another IDE window has the Serial Monitor open on that port; close it. |
| Serial Monitor shows garbage | Speed is not 115200. |
| `FATAL: WiFi timeout` (main firmware, after 20 s) or an endless line of dots (mic test) | Wrong name/password in `secrets.h`, or a 5 GHz network. Press `EN` after fixing. |
| `[WS] DISCONNECTED` repeating and the blue LED stays on (main firmware), `[WS] Disconnected` (mic test) | Server not running; wrong `VPS_HOST`; the Windows firewall rule from section 5 was not added; robot and PC on different networks; or `ROBOT_AUTH_TOKEN` is set on the server while the tested firmware is flashed (server window shows `Rejected ...: bad or missing token`). |
| Server: `Access is denied` | The Windows permission from section 5, step 2, is missing. |
| Server: `You must install or update .NET to run this application` | The .NET 8 SDK is not installed (section 2, step 3). |
| Server: `OPENAI_API_KEY is missing` | Type the `$env:OPENAI_API_KEY = "..."` line in the same window before `dotnet run`. |
| Server: `ffmpeg was not found` | Install ffmpeg, then close and reopen the server window (and type the key line again). |
| Server: `HTTP 401` | The key is wrong or was revoked. Create a new one. |
| Server: `HTTP 429` | No prepaid credit on your OpenAI account (add some under *Billing*), or too many requests in a row: wait a minute. |
| Three beeps, then nothing | No wake word in the sentence: check the `[STT] Transcription:` line in the server window to see what Whisper heard. Or the request failed: the error is in the server window. |
| One beep, then silence forever | You said only the wake word with no question (server shows `No command after wake word`), or the server hit an error after the wake beep. The tested firmware then stays muted until you press `EN` or re-plug the power. The experimental firmware fixes this. |
| The answer stutters or has gaps | Weak WiFi. Move closer to the router; make sure both sides are still at 8 kHz (the default). |
| The very end of every answer is missing | Known limitation of the tested firmware (the last ~0.1 s is cut). Fixed in `firmware/experimental/`. |
| The robot reboots when the answer plays | The power bank cannot deliver the peak current, or the amplifier's VIN is on 3V3 instead of VIN. Try another power bank or a shorter, thicker USB cable. |
| The board does not boot with the amplifier connected | Rare: pin 12 is a boot pin. Move the BCLK wire from D12 to D26 and change the pin number in the code: `I2S_SPK_SERIAL_CLOCK` in `esp32_voice_assistant.ino` and `I2S_SPK_BCLK` in `speaker_beep_test.ino`. |

## 12. Cost and privacy in one paragraph

Every sentence the robot hears is sent to OpenAI for transcription, even without
a wake word, and each answer costs a Whisper, a ChatGPT and a text-to-speech
call: a few cents per question at most, a few dollars for a month of playing.
The audio travels unencrypted between the robot and the server, so keep the
server on your home network or on a VPS with the port protected as in
section 10, and unplug the robot when you are not using it.
