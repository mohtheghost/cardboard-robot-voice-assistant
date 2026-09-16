# Experimental firmware

`esp32_voice_assistant_next/` is the working firmware plus the fixes found in a
code review. It compiles (ESP32 core 3.3.7, one harmless deprecation warning) but has
**not been flashed to the robot yet**. The main firmware in
`../esp32_voice_assistant/` stays the tested reference until this one is
verified on the hardware.

What it changes (each one is marked in the source with a comment):

| Change | Why |
|---|---|
| Sends `{"id":"esp32","token":"..."}` when `ROBOT_AUTH_TOKEN` is set in `secrets.h` | Lets the server reject strangers (`ROBOT_AUTH_TOKEN` on the server side). |
| Un-mutes the mic on `{"type":"listen"}` or after 20 s without an answer | The working firmware stays deaf until reboot if the server never answers after a wake beep. |
| Decodes each TTS chunk before waiting for queue space; the wait is a blocking `xQueueSend` instead of calling `webSocket.loop()` inside the WebSocket callback | The re-entrant call could overwrite the shared base64 buffer during long answers. |
| Mutes the mic while a beep plays and flushes the mic DMA afterwards | The robot's own beeps used to reach the server as "speech". |
| Waits 140 ms before zeroing the speaker DMA at the end of an answer; flushes the mic DMA before un-muting | The last ~130 ms of every answer was cut, and the mic sent the tail of the answer to the server. |
| Test beep off by default and blocked while an answer is in flight | It could slip in between two chunks of speech. |
| Reboots on WiFi timeout instead of halting | Power banks and hotspots are often not ready at boot. |
| Mic JSON is `{"type":"audio","data":...}` (no `target:"pc"`); buffer sized from the header constant | The PC relay is gone; the old fixed size 38 was the exact length of the old string. |
| All "8 kHz" strings print the real sample rate | The rate is 16 kHz. |

How to test it: flash it exactly like the main firmware (copy `secrets.h.example`
to `secrets.h` in this folder), start the server, and check in the serial monitor
that (1) the robot answers a question, (2) after saying only the wake word and
nothing else, the mic comes back within a second (`[MIC] ▶️ Microphone RESUMED
(server asked to listen)`), (3) a long answer (ask for a story) plays without
gaps or repeated words. If all three pass, move it over the main firmware.
