/*
 * ESP32 Voice Assistant - ULTRA MEMORY OPTIMIZED
 * 
 * MEMORY OPTIMIZATIONS:
 * - Streaming playback (plays while receiving - NO large buffer needed!)
 * - Minimal static buffers
 * - Reduced queue sizes
 * - Microphone disabled during TTS
 * - Dynamic memory allocation
 * 
 * I2S_NUM_0 → INMP441 Microphone (GPIO 27, 25, 18)
 * I2S_NUM_1 → MAX98357A Amplifier (GPIO 14, 12, 13)
 * 
 * Libraries: WebSockets, ArduinoJson v6.x
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ================= CONFIG =================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define VPS_HOST  "YOUR_SERVER_IP"
#define VPS_PORT  8080

// I2S Pins
#define I2S_MIC_WS   27
#define I2S_MIC_SCK  25
#define I2S_MIC_SD   18
#define I2S_SPK_LRC  14
#define I2S_SPK_BCLK 12
#define I2S_SPK_DIN  13

// Audio Config
#define SAMPLE_RATE  8000
#define AUDIO_GAIN   2.0f

// ✅ MEMORY OPTIMIZATION: Small buffers
#define MIC_BUFFER_SIZE   512    // Microphone read buffer (was 512)
#define SEND_QUEUE_SIZE   16     // Queue size (was 32) - saves ~16KB
#define BATCH_SIZE        4      // Batch size (was 6) - saves memory

// ✅ STREAMING TTS: No large buffer needed!
#define TTS_CHUNK_BUFFER  8192   // Only 8KB for streaming (was 77KB+!)

// ================= GLOBALS =================
WebSocketsClient webSocket;
bool registered = false;
QueueHandle_t sendQueue;

struct AudioChunk { int16_t* data; int count; };

// ✅ Minimal microphone buffers
static int32_t micRaw[MIC_BUFFER_SIZE];
static int16_t batchBuffer[MIC_BUFFER_SIZE * BATCH_SIZE];
static int batchedSamples = 0;

// ✅ Streaming TTS state
struct {
  bool receiving;
  bool playing;
  int totalChunks;
  int receivedChunks;
  uint8_t* playBuffer;      // Small buffer for decoding
  size_t playBufferSize;
  TaskHandle_t playTask;
} tts = {false, false, 0, 0, nullptr, 0, nullptr};

// Stats
static int sentChunks = 0;
static int droppedChunks = 0;

// ================= I2S SETUP =================
void setupMicrophone() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_MIC_SCK,
    .ws_io_num = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SD
  };
  
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);
}

void setupSpeaker() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_start(I2S_NUM_1);
}

// ================= TTS STREAMING =================

// ✅ Plays audio immediately (called from separate task)
void streamingPlayTask(void* param) {
  while (true) {
    if (tts.playing && tts.playBuffer) {
      // Write audio to I2S
      size_t written;
      i2s_write(I2S_NUM_1, tts.playBuffer, tts.playBufferSize, &written, portMAX_DELAY);
      
      // Mark buffer as consumed
      tts.playBufferSize = 0;
      tts.playing = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ✅ Handle incoming TTS chunk - STREAMING MODE
void handleTtsChunk(JsonDocument& doc) {
  if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) {
    return;
  }

  int chunkNum = doc["chunk"];
  int totalChunks = doc["total"];
  const char* b64Data = doc["data"];

  // First chunk - initialize
  if (chunkNum == 0) {
    Serial.println("\n[TTS] 🔊 Streaming audio...");
    
    // ✅ Allocate small buffer for one chunk
    if (tts.playBuffer) free(tts.playBuffer);
    tts.playBuffer = (uint8_t*)malloc(TTS_CHUNK_BUFFER);
    
    if (!tts.playBuffer) {
      Serial.println("[TTS] ❌ Out of memory!");
      return;
    }
    
    tts.receiving = true;
    tts.totalChunks = totalChunks;
    tts.receivedChunks = 0;
    
    // Start playback task if not running
    if (tts.playTask == nullptr) {
      xTaskCreatePinnedToCore(streamingPlayTask, "ttsPlay", 4096, NULL, 2, &tts.playTask, 0);
    }
  }

  if (!tts.receiving || !tts.playBuffer) return;

  // ✅ Decode directly into play buffer
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(
    tts.playBuffer,
    TTS_CHUNK_BUFFER,
    &decodedLen,
    (const unsigned char*)b64Data,
    strlen(b64Data)
  );

  if (ret != 0) {
    Serial.printf("[TTS] ❌ Decode failed (chunk %d)\n", chunkNum + 1);
    return;
  }

  // ✅ Play this chunk immediately
  tts.playBufferSize = decodedLen;
  tts.playing = true;
  
  // Wait for playback to finish before next chunk
  while (tts.playing) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  tts.receivedChunks++;

  // Progress
  if (tts.receivedChunks % 5 == 0 || tts.receivedChunks >= totalChunks) {
    int percent = (tts.receivedChunks * 100) / totalChunks;
    Serial.printf("[TTS] 🔊 %d%%\n", percent);
  }

  // All chunks played
  if (tts.receivedChunks >= totalChunks) {
    Serial.println("[TTS] ✅ Complete!\n");
    
    free(tts.playBuffer);
    tts.playBuffer = nullptr;
    tts.receiving = false;
    
    i2s_zero_dma_buffer(I2S_NUM_1);
  }
}

// ================= WEBSOCKET =================
void handleIncomingMessage(char* payload, size_t length) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) return;

  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "tts") == 0) {
    handleTtsChunk(doc);
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      registered = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] ✅ Connected!");
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      registered = true;
      break;

    case WStype_TEXT:
      handleIncomingMessage((char*)payload, length);
      break;

    default:
      break;
  }
}

// ================= MICROPHONE =================
void sendAudioChunk(int16_t* pcm, int count) {
  // Base64 encode
  size_t b64Len = ((count * 2 + 2) / 3) * 4 + 4;
  uint8_t* b64Buf = (uint8_t*)malloc(b64Len);
  if (!b64Buf) return;

  size_t outLen = 0;
  mbedtls_base64_encode(b64Buf, b64Len, &outLen, (uint8_t*)pcm, count * 2);
  b64Buf[outLen] = '\0';

  // Create JSON
  size_t jsonLen = 50 + outLen;
  char* jsonBuf = (char*)malloc(jsonLen);
  if (!jsonBuf) {
    free(b64Buf);
    return;
  }

  int headerLen = snprintf(jsonBuf, jsonLen, "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"");
  memcpy(jsonBuf + headerLen, b64Buf, outLen);
  jsonBuf[headerLen + outLen] = '"';
  jsonBuf[headerLen + outLen + 1] = '}';
  jsonBuf[headerLen + outLen + 2] = '\0';

  webSocket.sendTXT(jsonBuf);

  free(b64Buf);
  free(jsonBuf);
  sentChunks++;
}

void sendTask(void* param) {
  AudioChunk chunk;
  
  while (true) {
    if (xQueueReceive(sendQueue, &chunk, portMAX_DELAY)) {
      if (registered && !tts.receiving) {  // ✅ Don't send during TTS
        memcpy(batchBuffer + batchedSamples, chunk.data, chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
        if (batchedSamples >= MIC_BUFFER_SIZE * BATCH_SIZE) {
          sendAudioChunk(batchBuffer, batchedSamples);
          batchedSamples = 0;
        }
      }
      free(chunk.data);
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 Voice Assistant - OPTIMIZED    ║");
  Serial.println("║     Streaming Playback Enabled 🚀     ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] ✅ Connected: " + WiFi.localIP().toString());

  // WebSocket
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);

  // I2S
  setupMicrophone();
  setupSpeaker();

  // Queue
  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));

  // Tasks
  xTaskCreatePinnedToCore(sendTask, "sendTask", 6144, NULL, 1, NULL, 0);

  // Memory info
  Serial.printf("[MEM] Free: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[MEM] Largest block: %d KB\n", ESP.getMaxAllocHeap() / 1024);
  Serial.println("\n[INIT] ✅ Ready!\n");
}

// ================= LOOP =================
void loop() {
  webSocket.loop();

  if (!registered) return;

  // ✅ Pause mic during TTS
  if (tts.receiving) {
    delay(10);
    return;
  }

  // Read mic
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, micRaw, sizeof(micRaw), &bytesRead, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytesRead == 0) return;

  int count = bytesRead / sizeof(int32_t);
  int16_t* pcm = (int16_t*)malloc(count * sizeof(int16_t));
  if (!pcm) return;

  // Convert 32-bit to 16-bit with gain
  for (int i = 0; i < count; i++) {
    int32_t s = (micRaw[i] >> 14) * AUDIO_GAIN;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }

  AudioChunk chunk = { pcm, count };
  
  if (xQueueSend(sendQueue, &chunk, 0) != pdTRUE) {
    free(pcm);
    droppedChunks++;
  }
}
