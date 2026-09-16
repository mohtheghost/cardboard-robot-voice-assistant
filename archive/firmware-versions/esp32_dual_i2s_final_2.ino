/*
 * ESP32 Dual I2S Voice Assistant
 * 
 * I2S_NUM_0 → INMP441 Microphone (GPIO 27, 25, 18)
 * I2S_NUM_1 → MAX98357A Amplifier (GPIO 14, 12, 13)
 * 
 * Required Libraries:
 * - WebSockets by Markus Sattler
 * - ArduinoJson by Benoit Blanchon (v6.x)
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

// ---- DEBUG MODE ----
#define DEBUG_MODE true  // Set to false to reduce serial output

// ---- I2S 0: MICROPHONE (INMP441) ----
#define I2S_MIC_WS        27
#define I2S_MIC_SCK       25
#define I2S_MIC_SD        18
#define I2S_MIC_PORT      I2S_NUM_0

// ---- I2S 1: AMPLIFIER (MAX98357A) ----
#define I2S_SPK_LRC       14
#define I2S_SPK_BCLK      12
#define I2S_SPK_DIN       13
#define I2S_SPK_PORT      I2S_NUM_1

// ---- AUDIO CONFIG ----
#define SAMPLE_RATE    8000
#define DMA_BUF_LEN    512
#define DMA_BUF_COUNT  4
#define SAMPLE_BUFFER  DMA_BUF_LEN
#define AUDIO_GAIN     2.0f

// ---- QUEUE CONFIG ----
#define SEND_QUEUE_SIZE 32  // Reduced from 64 to save memory for TTS
#define BATCH_SIZE 6

// ---- TTS PLAYBACK CONFIG ----
#define MAX_AUDIO_SIZE  (150 * 1024)  // 150KB max (reduced from 300KB)

// ---- GLOBAL STATE ----
WebSocketsClient webSocket;
bool wsConnected = false;
bool registered  = false;

QueueHandle_t sendQueue;

struct AudioChunk {
  int16_t* data;
  int      count;
};

// --- RECORDING BUFFERS ---
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
static int messagesReceived = 0;  // ✅ Track incoming messages

// -------- I2S SETUP --------
void i2s_install_microphone() {
  const i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = true,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  
  const i2s_pin_config_t pins = {
    .bck_io_num   = I2S_MIC_SCK,
    .ws_io_num    = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };
  i2s_set_pin(I2S_MIC_PORT, &pins);
}

void i2s_install_speaker() {
  const i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  
  const i2s_pin_config_t pins = {
    .bck_io_num   = I2S_SPK_BCLK,
    .ws_io_num    = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_SPK_PORT, &pins);
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
      messagesReceived++;  // ✅ Count all incoming messages
      
      // ✅ Show ALL incoming messages for debugging
      if (DEBUG_MODE) {
        Serial.printf("\n[WS] ⬇️  Message #%d received (%d bytes)\n", messagesReceived, length);
        Serial.printf("[WS] First 100 chars: %.100s\n", payload);
      }
      
      // Handle incoming JSON (TTS audio)
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
    Serial.printf("[JSON] ❌ Parse failed: %s\n", error.c_str());
    if (DEBUG_MODE) {
      Serial.printf("[JSON] Raw message: %s\n", payload);
    }
    return;
  }

  if (DEBUG_MODE) {
    Serial.println("[JSON] ✅ Parse successful");
  }

  const char* type = doc["type"];
  
  if (!type) {
    Serial.println("[JSON] ⚠️  No 'type' field found");
    return;
  }
  
  if (DEBUG_MODE) {
    Serial.printf("[JSON] Message type: '%s'\n", type);
  }
  
  if (strcmp(type, "tts") == 0) {
    Serial.println("[JSON] ✅ TTS message detected - processing...");
    handleTtsChunk(doc);
  } else {
    Serial.printf("[JSON] ⚠️  Unknown type: '%s'\n", type);
  }
}

// ✅ -------- HANDLE TTS AUDIO CHUNKS --------
void handleTtsChunk(JsonDocument& doc) {
  // ✅ Check for required fields
  if (!doc.containsKey("chunk")) {
    Serial.println("[TTS] ❌ Missing 'chunk' field");
    return;
  }
  if (!doc.containsKey("total")) {
    Serial.println("[TTS] ❌ Missing 'total' field");
    return;
  }
  if (!doc.containsKey("data")) {
    Serial.println("[TTS] ❌ Missing 'data' field");
    return;
  }

  int chunkNum    = doc["chunk"];
  int totalChunks = doc["total"];
  const char* b64Data = doc["data"];

  Serial.printf("[TTS] 📦 Chunk %d/%d received\n", chunkNum + 1, totalChunks);

  if (!b64Data || strlen(b64Data) == 0) {
    Serial.println("[TTS] ❌ Data field is empty");
    return;
  }

  Serial.printf("[TTS] Data length: %d bytes (base64)\n", strlen(b64Data));

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
    
    // ✅ Check available heap memory
    size_t freeHeap = ESP.getFreeHeap();
    Serial.printf("[TTS] 💾 Free heap: %d KB\n", freeHeap / 1024);
    
    // ✅ Allocate based on available memory (leave 50KB for system)
    size_t requestedSize = MAX_AUDIO_SIZE;
    if (freeHeap < (requestedSize + 50 * 1024)) {
      requestedSize = freeHeap - 50 * 1024;
      Serial.printf("[TTS] ⚠️  Reducing buffer to %d KB\n", requestedSize / 1024);
    }
    
    tts.bufferSize = requestedSize;
    tts.bufferUsed = 0;
    
    tts.buffer = (uint8_t*)malloc(tts.bufferSize);
    
    if (!tts.buffer) {
      Serial.printf("[TTS] ❌ Out of memory! Tried to allocate %d KB\n", tts.bufferSize / 1024);
      Serial.printf("[TTS] 💾 Free heap was: %d KB\n", freeHeap / 1024);
      tts.receiving = false;
      return;
    }

    Serial.printf("[TTS] ✅ Buffer allocated: %d KB\n", tts.bufferSize / 1024);
    Serial.printf("[TTS] 📦 Expecting %d chunks\n", totalChunks);
  }

  if (!tts.receiving || !tts.buffer) return;

  // Decode base64
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

  // Progress
  if (tts.receivedChunks % 10 == 0 || tts.receivedChunks == tts.totalChunks) {
    int percent = (tts.receivedChunks * 100) / tts.totalChunks;
    Serial.printf("[TTS] 📥 Progress: %d%% (%d/%d)\n", 
                  percent, tts.receivedChunks, tts.totalChunks);
  }

  // All chunks received
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

// ✅ -------- PLAY TTS AUDIO VIA I2S_NUM_1 --------
void playTtsAudio() {
  if (!tts.readyToPlay || !tts.buffer) return;

  Serial.println("[TTS] 🔊 Playing audio through MAX98357A...");
  
  int16_t* pcm16 = (int16_t*)tts.buffer;
  size_t sampleCount = tts.bufferUsed / 2;
  
  size_t totalWritten = 0;
  size_t totalBytes = sampleCount * sizeof(int16_t);
  const size_t chunkSize = 2048;
  
  while (totalWritten < totalBytes) {
    size_t toWrite = min(chunkSize, totalBytes - totalWritten);
    size_t bytesWritten = 0;
    
    esp_err_t err = i2s_write(
      I2S_SPK_PORT,
      ((uint8_t*)pcm16) + totalWritten,
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

  i2s_zero_dma_buffer(I2S_SPK_PORT);
  
  Serial.println("\n╔════════════════════════════════════════════╗");
  Serial.println("║         ✅ PLAYBACK COMPLETE               ║");
  Serial.println("╚════════════════════════════════════════════╝\n");
  
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
        memcpy(batchBuffer + batchedSamples, chunk.data, chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
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
    Serial.printf("[STATS] Messages received from VPS: %d\n", messagesReceived);  // ✅ Show received count
    
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
  Serial.println("║   ESP32 Dual I2S Voice Assistant         ║");
  Serial.println("║   Microphone + Speaker (Separate Buses)   ║");
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

  // ✅ Install BOTH I2S peripherals
  i2s_install_microphone();
  i2s_install_speaker();
  
  i2s_start(I2S_MIC_PORT);
  i2s_start(I2S_SPK_PORT);

  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));
  if (sendQueue == NULL) {
    Serial.println("[ERROR] Failed to create queue!");
    while (1) delay(1000);
  }

  // Start tasks on Core 0
  xTaskCreatePinnedToCore(sendTask, "sendTask", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(ttsPlaybackTask, "ttsTask", 8192, NULL, 2, NULL, 0);

  Serial.println("[INIT] ✅ Dual I2S initialized");
  Serial.println("[INIT] 🎤 Mic:     I2S_0 (GPIO 27, 25, 18)");
  Serial.println("[INIT] 🔊 Speaker: I2S_1 (GPIO 14, 12, 13)");
  Serial.printf("[INIT] 💾 Free heap: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[INIT] 💾 Largest free block: %d KB\n", ESP.getMaxAllocHeap() / 1024);
  Serial.println("\n╔═══════════════════════════════════════════╗");
  Serial.println("║  ✅ READY TO RECEIVE AUDIO FROM PC       ║");
  Serial.println("║     Waiting for TTS messages...           ║");
  Serial.println("╚═══════════════════════════════════════════╝\n");
  
  lastStatsTime = millis();
}

// -------- LOOP (Core 1) - Microphone Recording --------
void loop() {
  webSocket.loop();
  printStats();

  if (!registered) return;

  // ✅ Pause microphone while receiving/playing TTS to save memory
  if (tts.receiving || tts.readyToPlay) {
    delay(10);
    return;
  }

  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_MIC_PORT, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(100));
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
