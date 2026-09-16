/*
 * ESP32 Voice Assistant - WITH SPEAKER TEST
 * 
 * This code will:
 * 1. Test speaker with beep on startup
 * 2. Then wait for TTS messages from PC
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

// Memory Optimization
#define MIC_BUFFER_SIZE   512
#define SEND_QUEUE_SIZE   16
#define BATCH_SIZE        4
#define TTS_CHUNK_BUFFER  8192

// ================= GLOBALS =================
WebSocketsClient webSocket;
bool registered = false;
QueueHandle_t sendQueue;

struct AudioChunk { int16_t* data; int count; };

static int32_t micRaw[MIC_BUFFER_SIZE];
static int16_t batchBuffer[MIC_BUFFER_SIZE * BATCH_SIZE];
static int batchedSamples = 0;

struct {
  bool receiving;
  bool playing;
  int totalChunks;
  int receivedChunks;
  uint8_t* playBuffer;
  size_t playBufferSize;
  TaskHandle_t playTask;
} tts = {false, false, 0, 0, nullptr, 0, nullptr};

static int sentChunks = 0;
static int droppedChunks = 0;

// ================= SPEAKER TEST =================
void testSpeaker() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║    🔊 SPEAKER HARDWARE TEST 🔊        ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  Serial.println("[TEST] Testing speaker wiring...");
  Serial.println("[TEST] You should hear a BEEP!");
  Serial.println();
  
  // Generate 1000 Hz beep
  int16_t samples[16];
  for (int i = 0; i < 16; i++) {
    float angle = (2.0 * PI * i) / 16.0;
    samples[i] = (int16_t)(sin(angle) * 15000);  // Loud beep!
  }
  
  Serial.println("[TEST] 🔊 Playing beep...");
  
  // Play for 2 seconds
  for (int i = 0; i < 1000; i++) {
    size_t written;
    i2s_write(I2S_NUM_1, samples, sizeof(samples), &written, portMAX_DELAY);
    
    if (i % 250 == 0) {
      Serial.printf("[TEST] %d%%\n", (i * 100) / 1000);
    }
  }
  
  i2s_zero_dma_buffer(I2S_NUM_1);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       ✅ SPEAKER TEST COMPLETE        ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  Serial.println("DID YOU HEAR THE BEEP?");
  Serial.println();
  Serial.println("✅ YES → Speaker works! Continuing to TTS mode...");
  Serial.println("❌ NO  → Check wiring before continuing!\n");
  
  delay(2000);
}

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
void streamingPlayTask(void* param) {
  while (true) {
    if (tts.playing && tts.playBuffer) {
      size_t written;
      i2s_write(I2S_NUM_1, tts.playBuffer, tts.playBufferSize, &written, portMAX_DELAY);
      
      tts.playBufferSize = 0;
      tts.playing = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void handleTtsChunk(JsonDocument& doc) {
  if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) {
    Serial.println("[TTS] ❌ Missing required fields");
    return;
  }

  int chunkNum = doc["chunk"];
  int totalChunks = doc["total"];
  const char* b64Data = doc["data"];

  // First chunk
  if (chunkNum == 0) {
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    🎵 RECEIVING AUDIO FROM PC         ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    Serial.println("[TTS] 🔊 Streaming audio...");
    
    if (tts.playBuffer) free(tts.playBuffer);
    tts.playBuffer = (uint8_t*)malloc(TTS_CHUNK_BUFFER);
    
    if (!tts.playBuffer) {
      Serial.println("[TTS] ❌ Out of memory!");
      return;
    }
    
    tts.receiving = true;
    tts.totalChunks = totalChunks;
    tts.receivedChunks = 0;
    
    Serial.printf("[TTS] 📦 Expecting %d chunks\n", totalChunks);
    
    if (tts.playTask == nullptr) {
      xTaskCreatePinnedToCore(streamingPlayTask, "ttsPlay", 4096, NULL, 2, &tts.playTask, 0);
    }
  }

  if (!tts.receiving || !tts.playBuffer) return;

  // Decode chunk
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(
    tts.playBuffer,
    TTS_CHUNK_BUFFER,
    &decodedLen,
    (const unsigned char*)b64Data,
    strlen(b64Data)
  );

  if (ret != 0) {
    Serial.printf("[TTS] ❌ Decode failed (chunk %d) - Error code: %d\n", chunkNum + 1, ret);
    Serial.printf("[TTS] 📊 Base64 length: %d\n", strlen(b64Data));
    return;
  }

  // Play chunk
  tts.playBufferSize = decodedLen;
  tts.playing = true;
  
  // Wait for playback
  int timeout = 0;
  while (tts.playing && timeout < 1000) {  // 5 second timeout
    vTaskDelay(pdMS_TO_TICKS(5));
    timeout++;
  }
  
  if (timeout >= 1000) {
    Serial.println("[TTS] ⚠️  Playback timeout!");
  }

  tts.receivedChunks++;

  // Progress
  if (tts.receivedChunks % 5 == 0 || tts.receivedChunks >= totalChunks) {
    int percent = (tts.receivedChunks * 100) / totalChunks;
    Serial.printf("[TTS] 🔊 Progress: %d%% (%d/%d)\n", percent, tts.receivedChunks, totalChunks);
  }

  // Complete
  if (tts.receivedChunks >= totalChunks) {
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║    ✅ PLAYBACK COMPLETE! ✅          ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    free(tts.playBuffer);
    tts.playBuffer = nullptr;
    tts.receiving = false;
    
    i2s_zero_dma_buffer(I2S_NUM_1);
  }
}

// ================= WEBSOCKET =================
void handleIncomingMessage(char* payload, size_t length) {
  Serial.printf("\n[WS] 📥 Received message: %d bytes\n", length);
  Serial.printf("[WS] First 100 chars: %.100s\n", payload);
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (error) {
    Serial.printf("[JSON] ❌ Parse failed: %s\n", error.c_str());
    return;
  }
  
  Serial.println("[JSON] ✅ Parse successful");

  const char* type = doc["type"];
  if (!type) {
    Serial.println("[JSON] ⚠️  No 'type' field");
    return;
  }
  
  Serial.printf("[JSON] Message type: '%s'\n", type);

  if (strcmp(type, "tts") == 0) {
    Serial.println("[JSON] ✅ TTS message detected!");
    handleTtsChunk(doc);
  } else {
    Serial.printf("[JSON] ⚠️  Unknown type: '%s'\n", type);
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] ❌ Disconnected from VPS");
      registered = false;
      break;

    case WStype_CONNECTED:
      Serial.println("\n[WS] ✅ Connected to VPS!");
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      registered = true;
      Serial.println("[WS] ✅ Registered as 'esp32'\n");
      break;

    case WStype_TEXT:
      handleIncomingMessage((char*)payload, length);
      break;

    case WStype_ERROR:
      Serial.println("[WS] ❌ WebSocket error!");
      break;

    default:
      break;
  }
}

// ================= MICROPHONE =================
void sendAudioChunk(int16_t* pcm, int count) {
  size_t b64Len = ((count * 2 + 2) / 3) * 4 + 4;
  uint8_t* b64Buf = (uint8_t*)malloc(b64Len);
  if (!b64Buf) return;

  size_t outLen = 0;
  mbedtls_base64_encode(b64Buf, b64Len, &outLen, (uint8_t*)pcm, count * 2);
  b64Buf[outLen] = '\0';

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
      if (registered && !tts.receiving) {
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
  delay(1000);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 Voice Assistant with Test      ║");
  Serial.println("║     Step 1: Speaker Test               ║");
  Serial.println("║     Step 2: TTS Reception              ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  // ✅ STEP 1: SPEAKER TEST
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("   STEP 1: TESTING SPEAKER HARDWARE");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  
  // Setup speaker first
  Serial.println("[I2S] Setting up speaker...");
  setupSpeaker();
  Serial.println("[I2S] ✅ Speaker initialized");
  Serial.printf("[I2S] Pins: LRC=%d, BCLK=%d, DIN=%d\n\n", I2S_SPK_LRC, I2S_SPK_BCLK, I2S_SPK_DIN);
  
  // Test speaker
  testSpeaker();

  // ✅ STEP 2: SETUP NETWORKING AND TTS
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.println("   STEP 2: SETTING UP TTS RECEPTION");
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  // WiFi
  Serial.println("[WiFi] Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] Signal: %d dBm\n\n", WiFi.RSSI());

  // WebSocket
  Serial.println("[WS] Connecting to VPS...");
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);
  Serial.println("[WS] ✅ WebSocket configured\n");

  // Microphone
  Serial.println("[I2S] Setting up microphone...");
  setupMicrophone();
  Serial.println("[I2S] ✅ Microphone initialized\n");

  // Queue
  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));

  // Tasks
  xTaskCreatePinnedToCore(sendTask, "sendTask", 6144, NULL, 1, NULL, 0);

  // Memory info
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
  Serial.printf("[MEM] Free heap: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[MEM] Largest block: %d KB\n", ESP.getMaxAllocHeap() / 1024);
  Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  ✅ ALL SYSTEMS READY!                ║");
  Serial.println("║                                        ║");
  Serial.println("║  Waiting for TTS from PC...            ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ================= LOOP =================
void loop() {
  webSocket.loop();

  if (!registered) return;

  // Pause mic during TTS
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
