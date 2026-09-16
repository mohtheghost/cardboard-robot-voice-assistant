/*
 * ESP32 VOICE ASSISTANT - BUFFER-THEN-PLAY (No Audio Cuts!)
 * ✅ 16kHz sample rate for clearer Arabic TTS
 * ✅ Periodic test beep (pauses during TTS)
 * ✅ Wake word confirmation beep (immediate feedback)
 * ✅ Progress beeps during download (25%, 50%, 75%, 100%)
 * ✅ Buffer complete TTS before playing (no cuts on slow networks!)
 */

#include <WiFi.h>
#include <ctype.h>
// Must appear BEFORE WebSocketsClient.h — PC sends multi-KB text frames per TTS chunk
#ifndef WEBSOCKETS_MAX_DATA_SIZE
#define WEBSOCKETS_MAX_DATA_SIZE (24 * 1024)
#endif
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ======================= CONFIG =======================

// LED
#define LED_PIN 2

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_DELAY_MS 500

// VPS / WebSocket
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
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
#define SPK_VOLUME_GAIN       1.5f

// ✅ TEST BEEP CONFIGURATION
#define BEEP_ENABLED          true   // Set to false to disable beep
#define BEEP_INTERVAL_MS      2000   // Beep every 2 seconds
#define BEEP_DURATION_MS      100    // Beep length (100ms)
#define BEEP_FREQUENCY        800    // 800 Hz tone
#define BEEP_VOLUME           0.01f  // 1% volume (very quiet test beep)

// ✅ WAKE WORD CONFIRMATION BEEP
#define WAKE_BEEP_DURATION_MS 200    // Slightly longer than test beep
#define WAKE_BEEP_FREQUENCY   1200   // Higher pitch (1200 Hz vs 800 Hz)
#define WAKE_BEEP_VOLUME      0.10f  // Louder than test beep (10% vs 1%)

// ✅ PROGRESS BEEPS (During TTS download)
#define PROGRESS_BEEP_DURATION_MS 50    // Very short beep
#define PROGRESS_BEEP_FREQUENCY   1000  // Medium pitch
#define PROGRESS_BEEP_VOLUME      0.05f // Quiet (5%)

// I2S DMA
#define I2S_DMA_BUF_COUNT     4
#define I2S_DMA_BUF_LEN       512

// Microphone
#define MIC_READ_BUFFER_SIZE  512
#define MIC_BATCH_SIZE        6      // Back to 6 (doesn't matter with buffer-then-play)
#define MIC_SEND_QUEUE_SIZE   32

// Speaker
#define SPK_WRITE_CHUNK_SIZE  2048
#define SPK_PLAY_QUEUE_SIZE   8      // Not used in buffer-then-play mode

// WebSocket send queue
#define WS_SEND_QUEUE_SIZE    16

// Tasks
#define TASK_MIC_SEND_STACK_SIZE      8192
#define TASK_MIC_SEND_PRIORITY        1
#define TASK_MIC_SEND_CORE            0

#define TASK_SPEAKER_PLAY_STACK_SIZE  4096
#define TASK_SPEAKER_PLAY_PRIORITY    2
#define TASK_SPEAKER_PLAY_CORE        0

// Debug
#define DEBUG_WEBSOCKET  true
#define DEBUG_MICROPHONE false
#define DEBUG_SPEAKER    false

// ✅ TTS BUFFER SETTINGS
#define MAX_TTS_BUFFER_SIZE   (150 * 1024)  // 150KB max (enough for long responses)

// ======================= TYPES =======================

struct MicrophoneChunk {
  int16_t* samples;
  size_t sampleCount;
};

struct SpeakerState {
  volatile bool isPlaying;
};

struct WsMessage {
  char* data;
};

// ✅ TTS BUFFER STATE
struct TtsBufferState {
  uint8_t* buffer;
  size_t bufferSize;
  size_t currentOffset;
  int chunksReceived;
  int totalChunks;
  uint32_t streamId;
  bool isActive;
  int lastProgressReported;  // For progress beeps
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
static uint8_t micBase64Buffer[MIC_B64_MAX_LEN];
static char micJsonBuffer[38 + MIC_B64_MAX_LEN + 3];

// Speaker
static SpeakerState speaker = { .isPlaying = false };
TaskHandle_t speakerPlaybackTask = nullptr;

// WS send queue
QueueHandle_t wsSendQueue;

// ✅ TTS BUFFER STATE (replaces streaming queue)
static TtsBufferState g_ttsBuffer = { 
  .buffer = nullptr, 
  .bufferSize = 0, 
  .currentOffset = 0,
  .chunksReceived = 0,
  .totalChunks = 0,
  .streamId = 0,
  .isActive = false,
  .lastProgressReported = 0
};

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

// ======================= BEEP FUNCTIONS =======================

// Generate a beep tone (generic function)
void generateBeep(int frequency, int durationMs, float volume) {
  // Calculate number of samples for the beep duration
  size_t numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
  
  // Allocate buffer for beep samples
  int16_t* beepBuffer = (int16_t*)malloc(numSamples * sizeof(int16_t));
  if (!beepBuffer) {
    Serial.println("[BEEP] ⚠️  malloc failed");
    return;
  }
  
  // Generate sine wave at specified frequency
  float phase = 0.0f;
  float phaseIncrement = (TWO_PI * frequency) / AUDIO_SAMPLE_RATE;
  
  for (size_t i = 0; i < numSamples; i++) {
    // Sine wave with envelope (fade in/out to avoid clicks)
    float envelope = 1.0f;
    
    // Fade in (first 5ms)
    if (i < (AUDIO_SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)i / (AUDIO_SAMPLE_RATE * 5 / 1000);
    }
    // Fade out (last 5ms)
    else if (i > numSamples - (AUDIO_SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)(numSamples - i) / (AUDIO_SAMPLE_RATE * 5 / 1000);
    }
    
    // Generate sine wave sample
    float sample = sinf(phase) * envelope * volume * 32767.0f;
    beepBuffer[i] = (int16_t)sample;
    
    // Increment phase
    phase += phaseIncrement;
    if (phase >= TWO_PI) phase -= TWO_PI;
  }
  
  // Write beep to speaker
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
  
  // Clean up
  free(beepBuffer);
}

// Test beep (quiet, low frequency)
void playTestBeep() {
  if (!BEEP_ENABLED) return;
  generateBeep(BEEP_FREQUENCY, BEEP_DURATION_MS, BEEP_VOLUME);
  g_lastBeepTime = millis();
}

// ✅ WAKE WORD CONFIRMATION BEEP (louder, higher frequency)
void playWakeWordBeep() {
  Serial.println("[WAKE] 🔔 Playing wake word confirmation beep");
  generateBeep(WAKE_BEEP_FREQUENCY, WAKE_BEEP_DURATION_MS, WAKE_BEEP_VOLUME);
}

// ✅ PROGRESS BEEP (short, quiet)
void playProgressBeep() {
  generateBeep(PROGRESS_BEEP_FREQUENCY, PROGRESS_BEEP_DURATION_MS, PROGRESS_BEEP_VOLUME);
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
  Serial.println("[MIC] Initializing INMP441 at 16kHz ...");

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

  Serial.println("[MIC] ✅ Microphone ready at 16kHz (high quality)");
  return true;
}

bool initializeSpeaker() {
  Serial.println("[SPK] Initializing MAX98357A at 16kHz ...");

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

  Serial.println("[SPK] ✅ Speaker ready at 16kHz (high quality)");
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
    "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\""
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
      if (wsRegistered && !speaker.isPlaying) {
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

// ======================= TTS BUFFER-THEN-PLAY =======================

// ✅ Initialize TTS buffer (first chunk)
bool initTtsBuffer(int totalChunks, uint32_t streamId) {
  // Free existing buffer if any
  if (g_ttsBuffer.buffer != nullptr) {
    free(g_ttsBuffer.buffer);
    g_ttsBuffer.buffer = nullptr;
  }

  // Estimate buffer size (assume ~4KB per chunk)
  size_t estimatedSize = totalChunks * 4096;
  if (estimatedSize > MAX_TTS_BUFFER_SIZE) {
    estimatedSize = MAX_TTS_BUFFER_SIZE;
  }

  // Allocate buffer
  g_ttsBuffer.buffer = (uint8_t*)malloc(estimatedSize);
  if (!g_ttsBuffer.buffer) {
    Serial.printf("[TTS] ❌ Failed to allocate %s buffer!\n", formatBytes(estimatedSize).c_str());
    return false;
  }

  // Initialize state
  g_ttsBuffer.bufferSize = estimatedSize;
  g_ttsBuffer.currentOffset = 0;
  g_ttsBuffer.chunksReceived = 0;
  g_ttsBuffer.totalChunks = totalChunks;
  g_ttsBuffer.streamId = streamId;
  g_ttsBuffer.isActive = true;
  g_ttsBuffer.lastProgressReported = 0;

  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║         📥 BUFFERING TTS AUDIO (Buffer-then-Play)        ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.printf("[TTS] 📦 Allocated %s buffer for %d chunks\n", 
                formatBytes(estimatedSize).c_str(), totalChunks);

  return true;
}

// ✅ Add chunk to buffer
bool addTtsChunk(const uint8_t* data, size_t length) {
  if (!g_ttsBuffer.isActive || !g_ttsBuffer.buffer) {
    return false;
  }

  // Check if buffer has space
  if (g_ttsBuffer.currentOffset + length > g_ttsBuffer.bufferSize) {
    // Try to expand buffer
    size_t newSize = g_ttsBuffer.bufferSize + length + 4096;
    if (newSize > MAX_TTS_BUFFER_SIZE) {
      Serial.println("[TTS] ❌ Buffer overflow! Response too large");
      return false;
    }

    uint8_t* newBuffer = (uint8_t*)realloc(g_ttsBuffer.buffer, newSize);
    if (!newBuffer) {
      Serial.println("[TTS] ❌ Failed to expand buffer");
      return false;
    }

    g_ttsBuffer.buffer = newBuffer;
    g_ttsBuffer.bufferSize = newSize;
    Serial.printf("[TTS] 📦 Expanded buffer to %s\n", formatBytes(newSize).c_str());
  }

  // Copy chunk to buffer
  memcpy(g_ttsBuffer.buffer + g_ttsBuffer.currentOffset, data, length);
  g_ttsBuffer.currentOffset += length;
  g_ttsBuffer.chunksReceived++;

  // Calculate progress
  int progress = (g_ttsBuffer.chunksReceived * 100) / g_ttsBuffer.totalChunks;

  // ✅ PLAY PROGRESS BEEPS at 25%, 50%, 75%, 100%
  if (progress >= 25 && g_ttsBuffer.lastProgressReported < 25) {
    Serial.println("[TTS] ▓░░░ 25% downloaded");
    playProgressBeep();
    g_ttsBuffer.lastProgressReported = 25;
  } else if (progress >= 50 && g_ttsBuffer.lastProgressReported < 50) {
    Serial.println("[TTS] ▓▓░░ 50% downloaded");
    playProgressBeep();
    g_ttsBuffer.lastProgressReported = 50;
  } else if (progress >= 75 && g_ttsBuffer.lastProgressReported < 75) {
    Serial.println("[TTS] ▓▓▓░ 75% downloaded");
    playProgressBeep();
    g_ttsBuffer.lastProgressReported = 75;
  } else if (progress >= 100 && g_ttsBuffer.lastProgressReported < 100) {
    Serial.println("[TTS] ▓▓▓▓ 100% downloaded");
    playProgressBeep();
    g_ttsBuffer.lastProgressReported = 100;
  } else if ((g_ttsBuffer.chunksReceived % 5) == 0) {
    // Show progress every 5 chunks
    Serial.printf("[TTS] 📥 %d%% (%d/%d chunks, %s)\n", 
                  progress, g_ttsBuffer.chunksReceived, g_ttsBuffer.totalChunks,
                  formatBytes(g_ttsBuffer.currentOffset).c_str());
  }

  return true;
}

// ✅ Play complete buffered audio
void playBufferedAudio() {
  if (!g_ttsBuffer.buffer || g_ttsBuffer.currentOffset == 0) {
    Serial.println("[TTS] ❌ No audio to play");
    return;
  }

  speaker.isPlaying = true;

  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║         🎵 PLAYING BUFFERED AUDIO (No Cuts!)             ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");
  Serial.printf("[TTS] 🔊 Playing %s of audio\n", formatBytes(g_ttsBuffer.currentOffset).c_str());

  // Apply volume gain
  int16_t* audioSamples = (int16_t*)g_ttsBuffer.buffer;
  size_t sampleCount = g_ttsBuffer.currentOffset / sizeof(int16_t);

  for (size_t i = 0; i < sampleCount; i++) {
    int32_t amplified = (int32_t)(audioSamples[i] * SPK_VOLUME_GAIN);
    
    if (amplified > 32767) amplified = 32767;
    if (amplified < -32768) amplified = -32768;
    
    audioSamples[i] = (int16_t)amplified;
  }

  // Write to speaker
  size_t totalWritten = 0;
  while (totalWritten < g_ttsBuffer.currentOffset) {
    size_t toWrite = min((size_t)SPK_WRITE_CHUNK_SIZE, g_ttsBuffer.currentOffset - totalWritten);
    size_t written = 0;

    esp_err_t r = i2s_write(I2S_NUM_1, g_ttsBuffer.buffer + totalWritten, toWrite, &written, portMAX_DELAY);
    if (r != ESP_OK) {
      Serial.printf("[TTS] ❌ i2s_write error: %d\n", r);
      break;
    }
    totalWritten += written;

    // Show progress every 10KB
    if ((totalWritten % 10240) == 0) {
      int playProgress = (totalWritten * 100) / g_ttsBuffer.currentOffset;
      Serial.printf("[TTS] 🔊 Playing: %d%% (%s/%s)\n", 
                    playProgress, formatBytes(totalWritten).c_str(), 
                    formatBytes(g_ttsBuffer.currentOffset).c_str());
    }
  }

  i2s_zero_dma_buffer(I2S_NUM_1);

  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════════════╗");
  Serial.println("║           ✅ PLAYBACK COMPLETE (Perfect Quality!)        ║");
  Serial.println("╚════════════════════════════════════════════════════════════╝");

  // Free buffer
  free(g_ttsBuffer.buffer);
  g_ttsBuffer.buffer = nullptr;
  g_ttsBuffer.isActive = false;

  speaker.isPlaying = false;

  // Reset beep timer (so test beep resumes after 2 seconds)
  g_lastBeepTime = millis();
}

// ✅ Handle TTS chunk (Buffer-then-Play mode)
void handleTtsAudioChunk(int chunkNumber, int totalChunks, const char* base64Data, size_t base64Len) {
  if (!base64Data || base64Len == 0 || totalChunks <= 0) return;

  // First chunk - initialize buffer
  if (chunkNumber == 0) {
    static uint32_t streamIdCounter = 0;
    streamIdCounter++;
    
    if (!initTtsBuffer(totalChunks, streamIdCounter)) {
      return;
    }
  }

  // Decode base64
  size_t maxDecoded = base64MaxDecodedLen(base64Len, base64Data);
  if (maxDecoded == 0) maxDecoded = 1;

  uint8_t* decoded = (uint8_t*)malloc(maxDecoded);
  if (!decoded) {
    Serial.printf("[TTS] ❌ malloc failed for chunk %d\n", chunkNumber);
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
    Serial.printf("[TTS] ❌ base64 decode failed r=%d\n", r);
    free(decoded);
    return;
  }

  // Add chunk to buffer
  if (!addTtsChunk(decoded, decodedLen)) {
    free(decoded);
    return;
  }

  free(decoded);

  // ✅ ALL CHUNKS RECEIVED - PLAY NOW!
  if (g_ttsBuffer.chunksReceived >= g_ttsBuffer.totalChunks) {
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════════════╗");
    Serial.println("║         ✅ ALL CHUNKS RECEIVED - STARTING PLAYBACK         ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝");
    
    // Small delay for last progress beep
    delay(100);
    
    playBufferedAudio();
  }
}

// ======================= WEBSOCKET =======================

void handleWebSocketMessage(char* payload, size_t length) {
  if (!payload || length == 0) return;

  // ✅ CHECK FOR WAKE WORD BEEP SIGNAL
  if (memContains(payload, length, "\"type\":\"wake_beep\"") || 
      memContains(payload, length, "\"type\": \"wake_beep\"")) {
    playWakeWordBeep();
    return;
  }

  // Check for TTS audio
  if (!memContains(payload, length, "\"type\":\"tts\"") && !memContains(payload, length, "\"type\": \"tts\""))
    return;

  int chunk = 0, total = 0;
  size_t b64Len = 0;
  if (!parseTtsJsonLite(payload, length, &chunk, &total, &b64Len)) {
    Serial.printf("[WS] TTS parse failed (len=%u)\n", (unsigned)length);
    return;
  }

  handleTtsAudioChunk(chunk, total, s_b64Work, b64Len);
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

      webSocket.sendTXT("{\"id\":\"esp32\"}");
      wsRegistered = true;
      
      Serial.println("[WS] ✅ ESP32 REGISTERED - Ready for 16kHz audio");
      Serial.println("[MODE] 🎯 Buffer-then-Play (No audio cuts!)");
      
      if (BEEP_ENABLED) {
        Serial.printf("[BEEP] 🔔 Test beep: %dHz, %dms, %.1f%% volume\n", 
                      BEEP_FREQUENCY, BEEP_DURATION_MS, BEEP_VOLUME * 100);
        Serial.printf("[BEEP] 🎯 Wake beep: %dHz, %dms, %.1f%% volume\n",
                      WAKE_BEEP_FREQUENCY, WAKE_BEEP_DURATION_MS, WAKE_BEEP_VOLUME * 100);
        Serial.printf("[BEEP] 📊 Progress beep: %dHz, %dms, %.1f%% volume\n",
                      PROGRESS_BEEP_FREQUENCY, PROGRESS_BEEP_DURATION_MS, PROGRESS_BEEP_VOLUME * 100);
      }
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
  Serial.println("║      ESP32 VOICE ASSISTANT - BUFFER-THEN-PLAY          ║");
  Serial.println("║           🔔 NO AUDIO CUTS ON ANY NETWORK! 🔔          ║");
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
      Serial.println("FATAL: WiFi timeout");
      while (1) delay(1000);
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
  wsSendQueue  = xQueueCreate(WS_SEND_QUEUE_SIZE, sizeof(WsMessage));

  if (!micSendQueue || !wsSendQueue) {
    Serial.println("FATAL: queue failed");
    while (1) delay(1000);
  }

  if (xTaskCreatePinnedToCore(microphoneSendTask, "MicSend", TASK_MIC_SEND_STACK_SIZE, NULL,
                             TASK_MIC_SEND_PRIORITY, NULL, TASK_MIC_SEND_CORE) != pdPASS) {
    Serial.println("FATAL: MicSend task failed");
    while (1) delay(1000);
  }

  Serial.println("\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║         ✅ BUFFER-THEN-PLAY MODE READY                   ║");
  Serial.println("║         🎯 Perfect Audio Quality, No Cuts!              ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝\n");
  
  Serial.printf("[INFO] Max TTS buffer size: %s\n", formatBytes(MAX_TTS_BUFFER_SIZE).c_str());
  Serial.printf("[INFO] Free heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
  
  if (BEEP_ENABLED) {
    Serial.println("\n[BEEP] 🔔 Speaker test beep will start in 2 seconds");
    Serial.println("[BEEP] ℹ️  Beeps continuously (even when disconnected)");
    Serial.println("[BEEP] ℹ️  Pauses automatically during TTS playback");
    Serial.println("[BEEP] 📊 Progress beeps show download status\n");
  }
  
  // ✅ Initialize beep timer
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

  // ✅ PERIODIC TEST BEEP (runs ALWAYS, even when disconnected)
  // Pauses automatically during TTS playback
  if (BEEP_ENABLED && !speaker.isPlaying) {
    unsigned long now = millis();
    if (now - g_lastBeepTime >= BEEP_INTERVAL_MS) {
      playTestBeep();
      // Timer is updated inside playTestBeep()
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
