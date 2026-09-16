/*
 * ESP32 Full Duplex Voice Assistant
 * 
 * Features:
 * - Records audio from INMP441 microphone
 * - Sends audio to PC via WebSocket
 * - Receives PCM audio from PC
 * - Plays audio through I2S DAC/Amplifier
 * 
 * Required Libraries (install via Arduino Library Manager):
 * - WebSockets by Markus Sattler (v2.3.6+)
 * - ArduinoJson by Benoit Blanchon (v6.x, NOT v7)
 * 
 * See ESP32_LIBRARIES.md for installation instructions
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ---- WIFI CONFIG ----
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// ---- VPS CONFIG ----
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080

// ---- I2S PINS ----
#define I2S_WS        27
#define I2S_SCK       25
#define I2S_SD_IN     18  // Microphone (INMP441)
#define I2S_SD_OUT    22  // ⚠️⚠️⚠️ SPEAKER OUTPUT - CHANGE THIS TO YOUR AMPLIFIER PIN! ⚠️⚠️⚠️
#define I2S_PORT_NUM  I2S_NUM_0

// Common speaker pins: 21, 22, 23, 19
// See ESP32_AMPLIFIER_WIRING.md for wiring details

// ---- AUDIO CONFIG ----
#define SAMPLE_RATE    8000
#define DMA_BUF_LEN    512
#define DMA_BUF_COUNT  4
#define SAMPLE_BUFFER  DMA_BUF_LEN
#define AUDIO_GAIN     2.0f

// ---- QUEUE CONFIG ----
#define SEND_QUEUE_SIZE 64
#define BATCH_SIZE 6

// ---- TTS PLAYBACK CONFIG ----
#define MAX_AUDIO_SIZE  (300 * 1024)  // Max 300KB audio buffer

// ---- GLOBAL STATE ----
WebSocketsClient webSocket;
bool wsConnected = false;
bool registered  = false;

QueueHandle_t sendQueue;

struct AudioChunk {
  int16_t* data;
  int      count;
};

// --- STATIC BUFFERS (Recording) ---
static int32_t  raw[SAMPLE_BUFFER];
#define B64_MAX_LEN (((SAMPLE_BUFFER * BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t  b64Buf[B64_MAX_LEN];
static char     jsonBuf[38 + B64_MAX_LEN + 3];
static int16_t  batchBuffer[SAMPLE_BUFFER * BATCH_SIZE];
static int      batchedSamples = 0;

// --- TTS PLAYBACK STATE ---
struct TtsPlayback {
  bool     receiving;
  bool     readyToPlay;
  int      totalChunks;
  int      receivedChunks;
  uint8_t* buffer;
  size_t   bufferSize;
  size_t   bufferUsed;
};

static TtsPlayback tts = { false, false, 0, 0, nullptr, 0, 0 };

// Statistics
static unsigned long lastStatsTime = 0;
static int droppedChunks = 0;
static int sentChunks = 0;

// -------- I2S SETUP (DUPLEX: Mic + Speaker) --------
void i2s_install() {
  const i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,  // 32-bit for INMP441 mic
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = true,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_driver_install(I2S_PORT_NUM, &cfg, 0, NULL);
}

void i2s_setpin() {
  const i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_SD_OUT,   // Speaker output
    .data_in_num  = I2S_SD_IN     // Microphone input
  };
  i2s_set_pin(I2S_PORT_NUM, &pins);
}

// -------- WEBSOCKET EVENTS --------
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      wsConnected = false;
      registered  = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] Connected to VPS!");
      wsConnected = true;
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      registered = true;
      Serial.printf("[CONFIG] Queue size: %d chunks (%.1fs buffer)\n", 
                    SEND_QUEUE_SIZE, 
                    (SEND_QUEUE_SIZE * SAMPLE_BUFFER) / (float)SAMPLE_RATE);
      Serial.printf("[CONFIG] Batch size: %d chunks per send\n", BATCH_SIZE);
      break;

    case WStype_TEXT:
      // ✅ Handle incoming JSON messages (including TTS audio)
      handleIncomingMessage((char*)payload, length);
      break;

    case WStype_ERROR:
      Serial.println("[WS] Error!");
      break;

    default:
      break;
  }
}

// ✅ -------- HANDLE INCOMING MESSAGES --------
void handleIncomingMessage(char* payload, size_t length) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    // Not JSON or parse error - might be a simple text message
    Serial.printf("[WS] Server: %s\n", payload);
    return;
  }

  const char* type = doc["type"];
  
  if (type && strcmp(type, "tts") == 0) {
    // ✅ TTS audio chunk received
    handleTtsChunk(doc);
  } else if (type) {
    Serial.printf("[MSG] Type: %s\n", type);
  }
}

// ✅ -------- HANDLE TTS AUDIO CHUNKS --------
void handleTtsChunk(JsonDocument& doc) {
  int chunkNum    = doc["chunk"];
  int totalChunks = doc["total"];
  const char* b64Data = doc["data"];

  if (!b64Data) {
    Serial.println("[TTS] ❌ Missing data field");
    return;
  }

  // First chunk - initialize buffer
  if (chunkNum == 0) {
    Serial.println("\n╔════════════════════════════════════════════╗");
    Serial.println("║       🎵 RECEIVING AUDIO FROM PC          ║");
    Serial.println("╚════════════════════════════════════════════╝");
    
    // Free old buffer if exists
    if (tts.buffer) {
      free(tts.buffer);
      tts.buffer = nullptr;
    }

    tts.receiving      = true;
    tts.readyToPlay    = false;
    tts.totalChunks    = totalChunks;
    tts.receivedChunks = 0;
    tts.bufferSize     = MAX_AUDIO_SIZE;
    tts.bufferUsed     = 0;
    
    // Allocate buffer for entire audio
    tts.buffer = (uint8_t*)malloc(tts.bufferSize);
    
    if (!tts.buffer) {
      Serial.println("[TTS] ❌ Out of memory!");
      tts.receiving = false;
      return;
    }

    Serial.printf("[TTS] 📦 Expecting %d chunks\n", totalChunks);
  }

  if (!tts.receiving || !tts.buffer) {
    return;
  }

  // Decode base64 chunk
  size_t b64Len = strlen(b64Data);
  size_t decodedLen = 0;
  
  int ret = mbedtls_base64_decode(
    tts.buffer + tts.bufferUsed,
    tts.bufferSize - tts.bufferUsed,
    &decodedLen,
    (const unsigned char*)b64Data,
    b64Len
  );

  if (ret != 0) {
    Serial.println("[TTS] ❌ Base64 decode failed");
    return;
  }

  tts.bufferUsed += decodedLen;
  tts.receivedChunks++;

  // Progress indicator
  if (tts.receivedChunks % 10 == 0 || tts.receivedChunks == tts.totalChunks) {
    int percent = (tts.receivedChunks * 100) / tts.totalChunks;
    Serial.printf("[TTS] 📥 Progress: %d%% (%d/%d)\n", 
                  percent, tts.receivedChunks, tts.totalChunks);
  }

  // All chunks received - ready to play
  if (tts.receivedChunks == tts.totalChunks) {
    tts.receiving = false;
    tts.readyToPlay = true;
    
    float durationSec = tts.bufferUsed / 2.0 / SAMPLE_RATE;
    
    Serial.println("\n╔════════════════════════════════════════════╗");
    Serial.println("║         ✅ AUDIO RECEIVED COMPLETE         ║");
    Serial.println("╚════════════════════════════════════════════╝");
    Serial.printf("[TTS] 📊 Size: %d bytes (%.1f KB)\n", tts.bufferUsed, tts.bufferUsed / 1024.0);
    Serial.printf("[TTS] ⏱️  Duration: %.1f seconds\n", durationSec);
    Serial.println("[TTS] 🔊 Ready for playback!\n");
  }
}

// ✅ -------- PLAY TTS AUDIO VIA I2S --------
void playTtsAudio() {
  if (!tts.readyToPlay || !tts.buffer) {
    return;
  }

  Serial.println("[TTS] 🔊 Playing audio through I2S...");
  
  // Convert 16-bit PCM to 32-bit for I2S output
  int16_t* pcm16 = (int16_t*)tts.buffer;
  int sampleCount = tts.bufferUsed / 2;
  
  // Allocate 32-bit buffer
  int32_t* pcm32 = (int32_t*)malloc(sampleCount * sizeof(int32_t));
  if (!pcm32) {
    Serial.println("[TTS] ❌ Failed to allocate playback buffer!");
    free(tts.buffer);
    tts.buffer = nullptr;
    tts.readyToPlay = false;
    return;
  }
  
  // Convert 16-bit to 32-bit (shift left 14 bits to match INMP441 format)
  for (int i = 0; i < sampleCount; i++) {
    pcm32[i] = ((int32_t)pcm16[i]) << 14;
  }
  
  size_t totalWritten = 0;
  size_t totalBytes = sampleCount * sizeof(int32_t);
  const size_t chunkSize = 2048;
  
  while (totalWritten < totalBytes) {
    size_t toWrite = min(chunkSize, totalBytes - totalWritten);
    size_t bytesWritten = 0;
    
    esp_err_t err = i2s_write(
      I2S_PORT_NUM,
      ((uint8_t*)pcm32) + totalWritten,
      toWrite,
      &bytesWritten,
      portMAX_DELAY
    );
    
    if (err != ESP_OK) {
      Serial.printf("[TTS] ❌ I2S write error: %d\n", err);
      break;
    }
    
    totalWritten += bytesWritten;
    
    // Progress every 25%
    int percentComplete = (totalWritten * 100) / totalBytes;
    static int lastPercent = -1;
    if (percentComplete != lastPercent && percentComplete % 25 == 0) {
      Serial.printf("[TTS] 🔊 Playing: %d%%\n", percentComplete);
      lastPercent = percentComplete;
    }
  }

  // Wait for I2S to finish
  i2s_zero_dma_buffer(I2S_PORT_NUM);
  
  Serial.println("\n╔════════════════════════════════════════════╗");
  Serial.println("║         ✅ PLAYBACK COMPLETE               ║");
  Serial.println("╚════════════════════════════════════════════╝\n");
  
  // Cleanup
  free(pcm32);
  free(tts.buffer);
  tts.buffer = nullptr;
  tts.readyToPlay = false;
}

// -------- SEND AUDIO CHUNK (Microphone) --------
void sendAudioChunk(int16_t* pcmData, int sampleCount) {
  size_t rawLen = sampleCount * sizeof(int16_t);

  size_t outputLen = 0;
  int ret = mbedtls_base64_encode(b64Buf, B64_MAX_LEN, &outputLen,
                                  (uint8_t*)pcmData, rawLen);
  if (ret != 0) {
    Serial.println("[ENCODE] Base64 failed!");
    return;
  }
  b64Buf[outputLen] = '\0';

  int headerLen = snprintf(jsonBuf, sizeof(jsonBuf),
                           "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"");
  memcpy(jsonBuf + headerLen, b64Buf, outputLen);
  jsonBuf[headerLen + outputLen]     = '"';
  jsonBuf[headerLen + outputLen + 1] = '}';
  jsonBuf[headerLen + outputLen + 2] = '\0';

  webSocket.sendTXT(jsonBuf);
  sentChunks++;
}

// -------- SEND TASK (Core 0) --------
void sendTask(void* param) {
  AudioChunk chunk;
  
  while (true) {
    if (xQueueReceive(sendQueue, &chunk, portMAX_DELAY)) {
      
      if (registered) {
        // Add chunk to batch buffer
        memcpy(batchBuffer + batchedSamples, chunk.data, chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
        // When we have a full batch, send it
        if (batchedSamples >= SAMPLE_BUFFER * BATCH_SIZE) {
          sendAudioChunk(batchBuffer, batchedSamples);
          batchedSamples = 0;
        }
      }
      
      free(chunk.data);
    }
  }
}

// ✅ -------- TTS PLAYBACK TASK (Core 0) --------
void ttsPlaybackTask(void* param) {
  while (true) {
    // Check if we should play audio
    if (tts.readyToPlay) {
      playTtsAudio();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// -------- PRINT STATS --------
void printStats() {
  unsigned long now = millis();
  if (now - lastStatsTime >= 10000) {
    int queueUsed = uxQueueMessagesWaiting(sendQueue);
    float queuePercent = (queueUsed * 100.0f) / SEND_QUEUE_SIZE;
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("[STATS] Queue usage: %d/%d (%.1f%%)\n", 
                  queueUsed, SEND_QUEUE_SIZE, queuePercent);
    Serial.printf("[STATS] Sent: %d chunks, Dropped: %d chunks\n", 
                  sentChunks, droppedChunks);
    
    if (droppedChunks > 0) {
      float dropRate = (droppedChunks * 100.0f) / (sentChunks + droppedChunks);
      Serial.printf("[STATS] ⚠️  Drop rate: %.2f%%\n", dropRate);
    }
    
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    
    sentChunks = 0;
    droppedChunks = 0;
    lastStatsTime = now;
  }
}

// -------- SETUP --------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n╔═══════════════════════════════════════════╗");
  Serial.println("║   ESP32 Voice Assistant (Full Duplex)    ║");
  Serial.println("║      Recording + Playback Enabled         ║");
  Serial.println("╚═══════════════════════════════════════════╝\n");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  Serial.printf("WiFi RSSI: %d dBm\n", WiFi.RSSI());

  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);

  // Setup I2S for BOTH microphone AND speaker
  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT_NUM);

  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));
  if (sendQueue == NULL) {
    Serial.println("[ERROR] Failed to create queue!");
    while (1) delay(1000);
  }

  // Start tasks on Core 0
  xTaskCreatePinnedToCore(sendTask, "sendTask", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ttsPlaybackTask, "ttsTask", 8192, NULL, 2, NULL, 0);

  Serial.println("[INIT] ✅ Dual-core: Recording + Playback active");
  Serial.printf("[INIT] 🎤 Microphone: GPIO %d\n", I2S_SD_IN);
  Serial.printf("[INIT] 🔊 Speaker: GPIO %d\n", I2S_SD_OUT);
  Serial.println();
  
  lastStatsTime = millis();
}

// -------- LOOP (Core 1) - Microphone Recording --------
void loop() {
  webSocket.loop();
  printStats();

  if (!registered) return;

  // ⚠️ Only read from microphone if NOT currently playing TTS
  // This prevents feedback and interference
  if (tts.readyToPlay || tts.receiving) {
    delay(10);
    return;
  }

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_PORT_NUM, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytes_read == 0) return;

  int count = bytes_read / sizeof(int32_t);

  int16_t* pcm = (int16_t*)malloc(count * sizeof(int16_t));
  if (!pcm) {
    Serial.println("[ERROR] malloc failed!");
    return;
  }

  for (int i = 0; i < count; i++) {
    int32_t s = raw[i] >> 14;
    s = s * AUDIO_GAIN;
    if      (s >  32767) s =  32767;
    else if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }

  AudioChunk chunk = { pcm, count };
  
  if (xQueueSend(sendQueue, &chunk, 0) != pdTRUE) {
    free(pcm);
    droppedChunks++;
  }
}
