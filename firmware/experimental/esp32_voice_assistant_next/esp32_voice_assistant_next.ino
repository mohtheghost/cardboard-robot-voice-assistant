/*
 * ESP32 CARDBOARD ROBOT - VOICE ASSISTANT FIRMWARE  ***EXPERIMENTAL***
 *
 * This is the working firmware (../../esp32_voice_assistant) plus fixes found in a
 * code review. It compiles in theory but has NOT been tested on the robot yet.
 * Differences from the working firmware:
 *   - credentials + optional auth token in secrets.h, sent as {"id":"esp32","token":...}
 *   - mic un-mutes on {"type":"listen"} from the server or after MIC_PAUSE_TIMEOUT_MS
 *     (the working firmware stays muted forever if the server never answers)
 *   - TTS chunks are decoded before waiting for queue space, and the wait no longer
 *     calls webSocket.loop() from inside the WebSocket callback (re-entrancy bug)
 *   - the microphone is muted while a beep plays and the mic DMA is flushed after it,
 *     so the robot's own beeps do not become 'speech' on the server
 *   - the last ~130 ms of an answer is no longer cut off; stale mic data is flushed
 *     before the mic un-mutes after an answer
 *   - the periodic test beep is off by default and cannot fire between TTS chunks
 *   - a WiFi timeout reboots the board instead of halting forever
 *   - mic JSON no longer carries the stale target:"pc" field; 8 kHz strings fixed
 *
 * The robot listens through an INMP441 I2S microphone, streams the audio to a
 * small C# server (see ../../server) over a WebSocket, and plays the server's
 * spoken answer through a MAX98357A I2S amplifier. All the "brains"
 * (speech-to-text, wake-word check, ChatGPT, text-to-speech) live on the server;
 * this firmware only moves audio and plays feedback beeps.
 *
 * Hardware (ESP32 DevKit V1, 30 pin):
 *   INMP441 mic   -> I2S_NUM_0 : SCK GPIO25, WS GPIO27, SD GPIO18, L/R -> GND, VDD 3V3
 *   MAX98357A amp -> I2S_NUM_1 : BCLK GPIO12, LRC GPIO14, DIN GPIO13, VIN 5V
 *   Status LED    -> GPIO2 (on-board LED): ON = not connected, OFF = connected
 *
 * Audio format: 16 kHz, 16-bit, mono PCM in both directions (base64 inside JSON).
 *
 * Feedback beeps:
 *   - 1 beep       : the server recognised the wake word and is answering
 *   - 3 quick beeps: the server stopped listening (end of your sentence)
 *   - optional faint beep every 2 s (BEEP_ENABLED) to check the speaker wiring
 *
 * The microphone is muted while the answer is playing so the robot does not
 * record itself; it un-mutes when playback ends, when the server sends
 * {"type":"listen"}, or after MIC_PAUSE_TIMEOUT_MS as a safety net.
 *
 * Libraries: "WebSockets" by Markus Sattler (arduinoWebSockets). Nothing else.
 * Copy secrets.h.example to secrets.h and fill in your WiFi / server details.
 */

#include <WiFi.h>
#include <ctype.h>
// Wanted receive limit for the WebSockets library (a TTS frame is ~2.8 KB). Must come before the
// include. Note: some library versions define this themselves (without #ifndef) and keep their own
// 15 KB limit and print a 'redefined' warning; both limits are plenty for this protocol.
#ifndef WEBSOCKETS_MAX_DATA_SIZE
#define WEBSOCKETS_MAX_DATA_SIZE (24 * 1024)
#endif
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ======================= CONFIG =======================

// LED
#define LED_PIN 2

// WiFi credentials, server address and optional auth token live in secrets.h
// (copy secrets.h.example -> secrets.h; secrets.h is git-ignored)
#include "secrets.h"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_DELAY_MS 500

// WebSocket
#define VPS_PATH "/"
#define WS_RECONNECT_INTERVAL_MS 500

// Heartbeat
#define WS_HEARTBEAT_INTERVAL_MS 15000
#define WS_HEARTBEAT_TIMEOUT_MS 30000
#define WS_HEARTBEAT_PONG_RETRIES 3

// I2S Mic (INMP441) - I2S_NUM_0
#define I2S_MIC_SERIAL_CLOCK  25
#define I2S_MIC_WORD_SELECT   27
#define I2S_MIC_SERIAL_DATA   18

// I2S Spk (MAX98357A) - I2S_NUM_1
#define I2S_SPK_SERIAL_CLOCK  12
#define I2S_SPK_WORD_SELECT   14
#define I2S_SPK_SERIAL_DATA   13

// Audio
#define AUDIO_SAMPLE_RATE     16000
#define MIC_I2S_BITS          32
#define SPK_I2S_BITS          16
#define MIC_GAIN_MULTIPLIER   1.5f
#define SPK_VOLUME_GAIN       3.0f

// ✅ TEST BEEP: a faint beep every 2 s. Handy to confirm the speaker wiring,
//    annoying in daily use and it is picked up by the microphone -> off by default.
#define BEEP_ENABLED          false
#define BEEP_INTERVAL_MS      2000
#define BEEP_DURATION_MS      100
#define BEEP_FREQUENCY        800
#define BEEP_VOLUME           0.01f

// ✅ WAKE WORD CONFIRMATION BEEP
#define WAKE_BEEP_DURATION_MS 200
#define WAKE_BEEP_FREQUENCY   1200
#define WAKE_BEEP_VOLUME      0.10f

// ✅ SPEECH-ENDED FEEDBACK BEEP (3 quick beeps, mid-high tone)
#define SPEECH_ENDED_BEEP_COUNT     3
#define SPEECH_ENDED_BEEP_DURATION  80
#define SPEECH_ENDED_BEEP_FREQUENCY 1100
#define SPEECH_ENDED_BEEP_VOLUME    0.08f
#define SPEECH_ENDED_BEEP_GAP       50

// ✅ MIC PAUSE SAFETY NET: after a wake beep the mic is muted until the answer has
//    played. If the server never sends an answer (API error, empty command...),
//    un-mute after this many ms so the robot does not stay deaf until reboot.
#define MIC_PAUSE_TIMEOUT_MS  20000

// I2S DMA
#define I2S_DMA_BUF_COUNT     4
#define I2S_DMA_BUF_LEN       512

// Microphone
#define MIC_READ_BUFFER_SIZE  512
#define MIC_BATCH_SIZE        3
#define MIC_SEND_QUEUE_SIZE   64

// Speaker
#define SPK_WRITE_CHUNK_SIZE  2048
#define SPK_PLAY_QUEUE_SIZE   16     // Queue for streaming

// WebSocket send queue
#define WS_SEND_QUEUE_SIZE    32

// Tasks
#define TASK_MIC_SEND_STACK_SIZE      8192
#define TASK_MIC_SEND_PRIORITY        1
#define TASK_MIC_SEND_CORE            0

#define TASK_SPEAKER_PLAY_STACK_SIZE  4096
#define TASK_SPEAKER_PLAY_PRIORITY    2
#define TASK_SPEAKER_PLAY_CORE        0

// Debug
#define DEBUG_WEBSOCKET  false
#define DEBUG_MICROPHONE false
#define DEBUG_SPEAKER    false

// ======================= TYPES =======================

struct MicrophoneChunk {
  int16_t* samples;
  size_t sampleCount;
};

struct SpeakerState {
  volatile bool isPlaying;
};

struct SpeakerChunk {
  uint8_t* data;
  size_t length;
  uint32_t streamId;
  int chunkNumber;
  int totalChunks;
};

struct WsMessage {
  char* data;
};

// ======================= GLOBALS =======================

WebSocketsClient webSocket;
volatile bool wsConnected = false;
volatile bool wsRegistered = false;

// Mic buffers
QueueHandle_t micSendQueue;
static int32_t micRawBuffer[MIC_READ_BUFFER_SIZE];
static int16_t micBatchBuffer[MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE];
static size_t micBatchedSamples = 0;

#define MIC_B64_MAX_LEN (((MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
#define MIC_JSON_HEADER "{\"type\":\"audio\",\"data\":\""
static uint8_t micBase64Buffer[MIC_B64_MAX_LEN];
static char micJsonBuffer[(sizeof(MIC_JSON_HEADER) - 1) + MIC_B64_MAX_LEN + 3];  // + closing quote, brace, NUL

// Speaker
static SpeakerState speaker = { .isPlaying = false };
QueueHandle_t spkPlayQueue;
TaskHandle_t speakerPlaybackTask = nullptr;

// ✅ Microphone control (pause during TTS to prevent self-recording)
volatile bool micEnabled = true;
static volatile bool g_localBeepPlaying = false;   // true while generateBeep() is writing to the amp
static volatile unsigned long g_micPausedAt = 0;   // millis() when the mic was muted / last TTS chunk arrived

// WS send queue
QueueHandle_t wsSendQueue;

// TTS stream tracking
static volatile uint32_t g_ttsStreamId = 0;

// ✅ TEST BEEP STATE
static unsigned long g_lastBeepTime = 0;

// ======================= UTILS =======================

void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW); }

void ledBlink(int times, int delayMs = 100) {
  for (int i = 0; i < times; i++) {
    ledOn();  delay(delayMs);
    ledOff(); delay(delayMs);
  }
}

String formatBytes(size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  if (bytes < 1024 * 1024) return String(bytes / 1024.0, 2) + " KB";
  return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

static inline size_t base64MaxDecodedLen(size_t base64Len, const char* s) {
  size_t out = (base64Len / 4) * 3;
  size_t pad = 0;
  if (base64Len >= 1 && s[base64Len - 1] == '=') pad++;
  if (base64Len >= 2 && s[base64Len - 2] == '=') pad++;
  if (out >= pad) out -= pad;
  return out;
}

void flushSpeakerQueue() {
  if (!spkPlayQueue) return;
  SpeakerChunk c;
  while (xQueueReceive(spkPlayQueue, &c, 0) == pdTRUE) {
    free(c.data);
  }
}

// ======================= BEEP FUNCTIONS =======================

// Throw away whatever the mic DMA ring captured (about 128 ms) - used after beeps and answers
static void discardMicDma() {
  static int32_t scratch[256];
  size_t got = 0;
  for (int i = 0; i < 8; i++) {
    if (i2s_read(I2S_NUM_0, scratch, sizeof(scratch), &got, pdMS_TO_TICKS(5)) != ESP_OK || got == 0) break;
  }
}

void generateBeep(int frequency, int durationMs, float volume) {
  g_localBeepPlaying = true;   // the mic task drops audio while this is set
  size_t numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;

  int16_t* beepBuffer = (int16_t*)malloc(numSamples * sizeof(int16_t));
  if (!beepBuffer) {
    Serial.println("[BEEP] ⚠️  malloc failed");
    g_localBeepPlaying = false;
    return;
  }

  float phase = 0.0f;
  float phaseIncrement = (TWO_PI * frequency) / AUDIO_SAMPLE_RATE;

  for (size_t i = 0; i < numSamples; i++) {
    float envelope = 1.0f;

    if (i < (AUDIO_SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)i / (AUDIO_SAMPLE_RATE * 5 / 1000);
    }
    else if (i > numSamples - (AUDIO_SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)(numSamples - i) / (AUDIO_SAMPLE_RATE * 5 / 1000);
    }

    float sample = sinf(phase) * envelope * volume * 32767.0f;
    beepBuffer[i] = (int16_t)sample;

    phase += phaseIncrement;
    if (phase >= TWO_PI) phase -= TWO_PI;
  }

  size_t totalWritten = 0;
  size_t totalBytes = numSamples * sizeof(int16_t);

  while (totalWritten < totalBytes) {
    size_t toWrite = min((size_t)SPK_WRITE_CHUNK_SIZE, totalBytes - totalWritten);
    size_t written = 0;

    esp_err_t r = i2s_write(I2S_NUM_1,
                           (uint8_t*)beepBuffer + totalWritten,
                           toWrite,
                           &written,
                           portMAX_DELAY);

    if (r != ESP_OK) {
      Serial.printf("[BEEP] ❌ i2s_write error: %d\n", r);
      break;
    }

    totalWritten += written;
  }

  free(beepBuffer);
  discardMicDma();             // the mic heard the beep: drop it
  g_localBeepPlaying = false;
}

void playTestBeep() {
  if (!BEEP_ENABLED) return;
  generateBeep(BEEP_FREQUENCY, BEEP_DURATION_MS, BEEP_VOLUME);
  g_lastBeepTime = millis();
}

void playWakeWordBeep() {
  Serial.println("[WAKE] 🔔 Playing wake word confirmation beep");
  generateBeep(WAKE_BEEP_FREQUENCY, WAKE_BEEP_DURATION_MS, WAKE_BEEP_VOLUME);

  // ✅ STOP MIC RECORDING (prevent self-recording during TTS)
  micEnabled = false;
  g_micPausedAt = millis();
  Serial.println("[MIC] ⏸️  Microphone PAUSED (will resume after TTS)");
}

void playSpeechEndedBeep() {
  Serial.println("[VAD] 🔕 Speech ended - playing confirmation beeps");
  for (int i = 0; i < SPEECH_ENDED_BEEP_COUNT; i++) {
    generateBeep(SPEECH_ENDED_BEEP_FREQUENCY, SPEECH_ENDED_BEEP_DURATION, SPEECH_ENDED_BEEP_VOLUME);
    if (i < SPEECH_ENDED_BEEP_COUNT - 1) {
      delay(SPEECH_ENDED_BEEP_GAP);
    }
  }
}

// ======================= TTS JSON (heap-free lite parse) =======================

static bool memContains(const char* p, size_t n, const char* sub) {
  size_t sl = strlen(sub);
  if (n < sl) return false;
  for (size_t i = 0; i + sl <= n; i++) {
    if (memcmp(p + i, sub, sl) == 0) return true;
  }
  return false;
}

static const char* findKey(const char* p, size_t n, const char* keyQuoted) {
  size_t kl = strlen(keyQuoted);
  if (n < kl) return nullptr;
  for (size_t i = 0; i + kl <= n; i++) {
    if (memcmp(p + i, keyQuoted, kl) == 0) return p + i + kl;
  }
  return nullptr;
}

static const char* skipWs(const char* p, const char* end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static bool readIntAfterKey(const char* p, const char* end, int* out) {
  p = skipWs(p, end);
  if (p >= end || *p != ':') return false;
  p = skipWs(p + 1, end);
  if (p >= end) return false;
  int sign = 1;
  if (*p == '-') { sign = -1; p++; }
  if (p >= end || !isdigit((unsigned char)*p)) return false;
  long v = 0;
  while (p < end && isdigit((unsigned char)*p)) {
    v = v * 10 + (*p - '0');
    p++;
  }
  *out = (int)(sign * v);
  return true;
}

static bool isBase64Char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

#define TTS_B64_WORK_SIZE 12288
static char s_b64Work[TTS_B64_WORK_SIZE];

static bool extractDataFieldBase64(const char* p, size_t n, size_t* outLen) {
  const char* end = p + n;
  const char* k = findKey(p, n, "\"data\"");
  if (!k) return false;
  k = skipWs(k, end);
  if (k >= end || *k != ':') return false;
  k = skipWs(k + 1, end);
  if (k >= end || *k != '"') return false;
  k++;

  size_t w = 0;
  while (k < end) {
    if (*k == '"') break;

    if (*k == '\\') {
      k++;
      if (k >= end) return false;
      if (*k == '/') {
        if (w >= sizeof(s_b64Work)) return false;
        s_b64Work[w++] = '/';
        k++;
        continue;
      }
      if (*k == '\\') {
        if (w >= sizeof(s_b64Work)) return false;
        s_b64Work[w++] = '\\';
        k++;
        continue;
      }
      if (*k == '"') {
        if (w >= sizeof(s_b64Work)) return false;
        s_b64Work[w++] = '"';
        k++;
        continue;
      }
      if (*k == 'u' && (size_t)(end - k) > 4) {
        k++;
        unsigned val = 0;
        for (int i = 0; i < 4; i++) {
          if (k >= end) return false;
          char h = *k++;
          unsigned d;
          if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') d = 10u + (unsigned)(h - 'a');
          else if (h >= 'A' && h <= 'F') d = 10u + (unsigned)(h - 'A');
          else return false;
          val = val * 16u + d;
        }
        if (val > 127u) return false;
        unsigned char ch = (unsigned char)val;
        if (!isBase64Char(ch)) return false;
        if (w >= sizeof(s_b64Work)) return false;
        s_b64Work[w++] = (char)ch;
        continue;
      }
      return false;
    }

    unsigned char c = (unsigned char)*k++;
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    if (!isBase64Char(c)) return false;
    if (w >= sizeof(s_b64Work)) return false;
    s_b64Work[w++] = (char)c;
  }
  if (k >= end || *k != '"') return false;
  *outLen = w;
  return w > 0;
}

static bool parseTtsJsonLite(const char* p, size_t n, int* chunk, int* total, size_t* b64Len) {
  const char* end = p + n;

  const char* c = findKey(p, n, "\"chunk\"");
  if (!c || !readIntAfterKey(c, end, chunk)) return false;

  const char* t = findKey(p, n, "\"total\"");
  if (!t || !readIntAfterKey(t, end, total)) return false;

  return extractDataFieldBase64(p, n, b64Len);
}

// ======================= I2S INIT =======================

bool initializeMicrophone() {
  Serial.printf("[MIC] Initializing INMP441 at %d Hz ...\n", AUDIO_SAMPLE_RATE);

  i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)MIC_I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_MIC_SERIAL_CLOCK,
    .ws_io_num = I2S_MIC_WORD_SELECT,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SERIAL_DATA
  };
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t r = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_driver_install failed: %d\n", r); return false; }

  r = i2s_set_pin(I2S_NUM_0, &pinConfig);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_set_pin failed: %d\n", r); return false; }

  r = i2s_start(I2S_NUM_0);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_start failed: %d\n", r); return false; }

  Serial.printf("[MIC] ✅ Microphone ready (%d Hz, 32-bit I2S -> 16-bit PCM)\n", AUDIO_SAMPLE_RATE);
  return true;
}

bool initializeSpeaker() {
  Serial.printf("[SPK] Initializing MAX98357A at %d Hz ...\n", AUDIO_SAMPLE_RATE);

  i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SPK_I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_SPK_SERIAL_CLOCK,
    .ws_io_num = I2S_SPK_WORD_SELECT,
    .data_out_num = I2S_SPK_SERIAL_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t r = i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_driver_install failed: %d\n", r); return false; }

  r = i2s_set_pin(I2S_NUM_1, &pinConfig);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_set_pin failed: %d\n", r); return false; }

  r = i2s_start(I2S_NUM_1);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_start failed: %d\n", r); return false; }

  Serial.printf("[SPK] ✅ Speaker ready (%d Hz, 16-bit mono)\n", AUDIO_SAMPLE_RATE);
  return true;
}

// ======================= WS SEND =======================

void queueWsSend(const char* s) {
  if (!wsSendQueue) return;

  size_t len = strlen(s);
  char* buf = (char*)malloc(len + 1);
  if (!buf) return;
  memcpy(buf, s, len + 1);

  WsMessage m = { buf };
  if (xQueueSend(wsSendQueue, &m, 0) != pdTRUE) {
    free(buf);
  }
}

// ======================= MICROPHONE =======================

void sendMicrophoneChunkQueued(int16_t* samples, size_t sampleCount) {
  if (!wsConnected || !wsRegistered) return;

  size_t pcmBytes = sampleCount * sizeof(int16_t);

  size_t base64Length = 0;
  int r = mbedtls_base64_encode(
    micBase64Buffer,
    MIC_B64_MAX_LEN,
    &base64Length,
    (uint8_t*)samples,
    pcmBytes
  );
  if (r != 0) return;

  micBase64Buffer[base64Length] = '\0';

  int headerLength = snprintf(
    micJsonBuffer,
    sizeof(micJsonBuffer),
    MIC_JSON_HEADER
  );

  memcpy(micJsonBuffer + headerLength, micBase64Buffer, base64Length);
  micJsonBuffer[headerLength + base64Length] = '"';
  micJsonBuffer[headerLength + base64Length + 1] = '}';
  micJsonBuffer[headerLength + base64Length + 2] = '\0';

  queueWsSend(micJsonBuffer);

  if (DEBUG_MICROPHONE) {
    static uint32_t n = 0;
    if ((++n % 20) == 0) Serial.printf("[MIC] queued %lu\n", (unsigned long)n);
  }
}

void microphoneSendTask(void* parameter) {
  MicrophoneChunk chunk;

  while (true) {
    if (xQueueReceive(micSendQueue, &chunk, portMAX_DELAY) == pdTRUE) {
      if (g_localBeepPlaying) {
        micBatchedSamples = 0;   // never glue pre-beep audio to post-beep audio
      } else if (wsRegistered && !speaker.isPlaying && micEnabled) {
        memcpy(micBatchBuffer + micBatchedSamples, chunk.samples, chunk.sampleCount * sizeof(int16_t));
        micBatchedSamples += chunk.sampleCount;

        if (micBatchedSamples >= (MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE)) {
          sendMicrophoneChunkQueued(micBatchBuffer, micBatchedSamples);
          micBatchedSamples = 0;
        }
      }
      free(chunk.samples);
    }
  }
}

// ======================= SPEAKER (plays TTS chunks as they arrive) =======================

void handleTtsAudioChunkParsed(int chunkNumber, int totalChunks,
                               const char* base64Data, size_t base64Len) {
  if (!base64Data || base64Len == 0 || totalChunks <= 0) return;

  g_micPausedAt = millis();   // answer is flowing: keep the mic muted, reset the safety-net timer

  if (chunkNumber == 0) {
    g_ttsStreamId++;
    flushSpeakerQueue();

    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════════════╗");
    Serial.println("║         🎵 STREAMING ANSWER FROM SERVER                   ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝");
    Serial.printf("[SPK] New stream id=%lu total=%d chunks\n", (unsigned long)g_ttsStreamId, totalChunks);
  }

  // Decode straight away: base64Data points at a shared static buffer that the next
  // WebSocket frame would overwrite. Waiting for queue space happens below, on the
  // heap copy, by blocking in xQueueSend (TCP flow control then throttles the server).
  size_t maxDecoded = base64MaxDecodedLen(base64Len, base64Data);
  if (maxDecoded == 0) maxDecoded = 1;

  uint8_t* decoded = (uint8_t*)malloc(maxDecoded);
  if (!decoded) {
    Serial.printf("[SPK] malloc failed (%s) freeHeap=%u\n",
                  formatBytes(maxDecoded).c_str(), (unsigned)ESP.getFreeHeap());
    return;
  }

  size_t decodedLen = 0;
  int r = mbedtls_base64_decode(
    decoded,
    maxDecoded,
    &decodedLen,
    (const unsigned char*)base64Data,
    base64Len
  );

  if (r != 0 || decodedLen == 0) {
    Serial.printf("[SPK] base64 decode failed r=%d\n", r);
    free(decoded);
    return;
  }

  SpeakerChunk c;
  c.data = decoded;
  c.length = decodedLen;
  c.streamId = g_ttsStreamId;
  c.chunkNumber = chunkNumber;
  c.totalChunks = totalChunks;

  // The speaker task frees one slot every 64 ms (2048 bytes at 16 kHz); 5 s means it is stuck.
  if (xQueueSend(spkPlayQueue, &c, pdMS_TO_TICKS(5000)) != pdTRUE) {
    free(decoded);
    Serial.println("[SPK] ERROR: enqueue failed (speaker task stuck?)");
    return;
  }

  if ((chunkNumber + 1) % 10 == 0 || (chunkNumber + 1) == totalChunks) {
    int progress = ((chunkNumber + 1) * 100) / totalChunks;
    Serial.printf("[SPK] Progress: %d%% (%d/%d, %s)\n",
                  progress, chunkNumber + 1, totalChunks, formatBytes(decodedLen).c_str());
  }
}

void speakerPlaybackTaskFunction(void* parameter) {
  SpeakerChunk c;

  while (true) {
    if (xQueueReceive(spkPlayQueue, &c, portMAX_DELAY) == pdTRUE) {

      if (c.streamId != g_ttsStreamId) {
        free(c.data);
        continue;
      }

      speaker.isPlaying = true;

      int16_t* audioSamples = (int16_t*)c.data;
      size_t sampleCount = c.length / sizeof(int16_t);

      for (size_t i = 0; i < sampleCount; i++) {
        int32_t amplified = (int32_t)(audioSamples[i] * SPK_VOLUME_GAIN);

        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;

        audioSamples[i] = (int16_t)amplified;
      }

      size_t totalWritten = 0;
      while (totalWritten < c.length) {
        size_t toWrite = min((size_t)SPK_WRITE_CHUNK_SIZE, c.length - totalWritten);
        size_t written = 0;

        esp_err_t r = i2s_write(I2S_NUM_1, c.data + totalWritten, toWrite, &written, portMAX_DELAY);
        if (r != ESP_OK) {
          Serial.printf("[SPK] i2s_write error: %d\n", r);
          break;
        }
        totalWritten += written;
      }

      if (c.chunkNumber + 1 >= c.totalChunks) {
        vTaskDelay(pdMS_TO_TICKS(140));   // i2s_write returns before the DMA ring (~128 ms) has played
        i2s_zero_dma_buffer(I2S_NUM_1);
        discardMicDma();                  // the mic ring holds the end of the answer: drop it
        Serial.println();
        Serial.println("╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           ✅ STREAMING PLAYBACK COMPLETE                   ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");

        // ✅ RESUME MIC RECORDING (TTS finished, safe to record again)
        micEnabled = true;
        Serial.println("[MIC] ▶️  Microphone RESUMED (ready for next command)");

        g_lastBeepTime = millis();
      }

      free(c.data);
      speaker.isPlaying = false;
    }
  }
}

// ======================= WEBSOCKET =======================

void handleWebSocketMessage(char* payload, size_t length) {
  if (!payload || length == 0) return;

  if (memContains(payload, length, "\"type\":\"wake_beep\"") ||
      memContains(payload, length, "\"type\": \"wake_beep\"")) {
    playWakeWordBeep();
    return;
  }

  if (memContains(payload, length, "\"type\":\"speech_ended\"") ||
      memContains(payload, length, "\"type\": \"speech_ended\"")) {
    playSpeechEndedBeep();
    return;
  }

  // Server gave up on this request (no command / API error): listen again right away
  if (memContains(payload, length, "\"type\":\"listen\"") ||
      memContains(payload, length, "\"type\": \"listen\"")) {
    micEnabled = true;
    Serial.println("[MIC] ▶️  Microphone RESUMED (server asked to listen)");
    return;
  }

  if (!memContains(payload, length, "\"type\":\"tts\"") && !memContains(payload, length, "\"type\": \"tts\""))
    return;

  int chunk = 0, total = 0;
  size_t b64Len = 0;
  if (!parseTtsJsonLite(payload, length, &chunk, &total, &b64Len)) {
    Serial.printf("[WS] TTS parse failed (len=%u)\n", (unsigned)length);
    return;
  }

  handleTtsAudioChunkParsed(chunk, total, s_b64Work, b64Len);
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("\n[WS] ❌ DISCONNECTED");
      ledOn();
      wsConnected = false;
      wsRegistered = false;
      break;

    case WStype_CONNECTED:
      Serial.printf("\n[WS] ✅ CONNECTED: %s\n", payload);
      ledOff();
      wsConnected = true;

      {
        char reg[128];
        if (strlen(ROBOT_AUTH_TOKEN) > 0)
          snprintf(reg, sizeof(reg), "{\"id\":\"esp32\",\"token\":\"%s\"}", ROBOT_AUTH_TOKEN);
        else
          snprintf(reg, sizeof(reg), "{\"id\":\"esp32\"}");
        webSocket.sendTXT(reg);
      }
      wsRegistered = true;

      Serial.printf("[WS] ✅ ESP32 REGISTERED - streaming microphone at %d Hz\n", AUDIO_SAMPLE_RATE);
      Serial.printf("[MEM] Free heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
      break;

    case WStype_TEXT:
      handleWebSocketMessage((char*)payload, length);
      break;

    case WStype_PING:
      if (DEBUG_WEBSOCKET) Serial.println("[WS] 🏓 PING");
      break;

    case WStype_PONG:
      if (DEBUG_WEBSOCKET) Serial.println("[WS] 🏓 PONG");
      break;

    case WStype_ERROR:
      Serial.println("[WS] ❌ ERROR");
      ledBlink(3, 100);
      break;

    default:
      break;
  }
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║      ESP32 CARDBOARD ROBOT - VOICE ASSISTANT           ║");
  Serial.println("║      mic -> server (STT + GPT + TTS) -> speaker        ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝\n");

  pinMode(LED_PIN, OUTPUT);
  ledOff();
  ledBlink(2, 150);

  if (!initializeSpeaker()) {
    Serial.println("FATAL: speaker init failed");
    while (1) delay(1000);
  }
  if (!initializeMicrophone()) {
    Serial.println("FATAL: mic init failed");
    while (1) delay(1000);
  }

  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi timeout - restarting (is the hotspot/router up yet?)");
      delay(1000);
      ESP.restart();
    }
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected! IP=%s\n", WiFi.localIP().toString().c_str());

  Serial.printf("[WS] Connecting to %s:%d\n", VPS_HOST, VPS_PORT);
  webSocket.begin(VPS_HOST, VPS_PORT, VPS_PATH);
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);

  if (WS_HEARTBEAT_INTERVAL_MS > 0) {
    webSocket.enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS,
                              WS_HEARTBEAT_TIMEOUT_MS,
                              WS_HEARTBEAT_PONG_RETRIES);
  }

  ledOn();

  micSendQueue = xQueueCreate(MIC_SEND_QUEUE_SIZE, sizeof(MicrophoneChunk));
  spkPlayQueue = xQueueCreate(SPK_PLAY_QUEUE_SIZE, sizeof(SpeakerChunk));
  wsSendQueue  = xQueueCreate(WS_SEND_QUEUE_SIZE, sizeof(WsMessage));

  if (!micSendQueue || !spkPlayQueue || !wsSendQueue) {
    Serial.println("FATAL: queue failed");
    while (1) delay(1000);
  }

  if (xTaskCreatePinnedToCore(microphoneSendTask, "MicSend", TASK_MIC_SEND_STACK_SIZE, NULL,
                             TASK_MIC_SEND_PRIORITY, NULL, TASK_MIC_SEND_CORE) != pdPASS) {
    Serial.println("FATAL: MicSend task failed");
    while (1) delay(1000);
  }

  if (xTaskCreatePinnedToCore(speakerPlaybackTaskFunction, "SpkPlay", TASK_SPEAKER_PLAY_STACK_SIZE, NULL,
                             TASK_SPEAKER_PLAY_PRIORITY, &speakerPlaybackTask, TASK_SPEAKER_PLAY_CORE) != pdPASS) {
    Serial.println("FATAL: SpkPlay task failed");
    while (1) delay(1000);
  }

  Serial.println("\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║         ✅ READY - say the wake word                    ║");
  Serial.println("║                                                          ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝\n");

  Serial.printf("[INFO] Sample rate: %d Hz\n", AUDIO_SAMPLE_RATE);
  Serial.printf("[INFO] Free heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());

  g_lastBeepTime = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 2000) {
      WiFi.reconnect();
      lastTry = millis();
    }
  }

  webSocket.loop();

  WsMessage m;
  while (xQueueReceive(wsSendQueue, &m, 0) == pdTRUE) {
    if (wsConnected && wsRegistered) {
      webSocket.sendTXT(m.data);
    }
    free(m.data);
  }

  // Safety net: mic muted by a wake beep but no answer ever came -> listen again
  if (!micEnabled && !speaker.isPlaying && (millis() - g_micPausedAt) > MIC_PAUSE_TIMEOUT_MS) {
    micEnabled = true;
    Serial.println("[MIC] ▶️  Microphone RESUMED (no answer from server within timeout)");
  }

  if (BEEP_ENABLED && !speaker.isPlaying && micEnabled) {   // micEnabled is false for the whole answer
    unsigned long now = millis();
    if (now - g_lastBeepTime >= BEEP_INTERVAL_MS) {
      playTestBeep();
    }
  }

  if (!wsConnected) {
    delay(5);
    return;
  }

  if (speaker.isPlaying) {
    delay(1);
    return;
  }

  size_t bytesRead = 0;
  esp_err_t r = i2s_read(I2S_NUM_0, micRawBuffer, sizeof(micRawBuffer), &bytesRead, pdMS_TO_TICKS(100));
  if (r != ESP_OK || bytesRead == 0) {
    delay(1);
    return;
  }

  size_t sampleCount = bytesRead / sizeof(int32_t);

  int16_t* converted = (int16_t*)malloc(sampleCount * sizeof(int16_t));
  if (!converted) {
    delay(1);
    return;
  }

  for (size_t i = 0; i < sampleCount; i++) {
    int32_t s = micRawBuffer[i] >> 14;
    s = (int32_t)(s * MIC_GAIN_MULTIPLIER);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    converted[i] = (int16_t)s;
  }

  MicrophoneChunk c = { converted, sampleCount };
  if (xQueueSend(micSendQueue, &c, 0) != pdTRUE) {
    free(converted);
  }
}
