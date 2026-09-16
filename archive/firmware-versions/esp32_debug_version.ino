/*
 * ESP32 VOICE ASSISTANT - DEBUG VERSION
 * 
 * This version has DETAILED LOGGING to find the problem!
 * Same code as literal combination, but with debug prints everywhere
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// VPS
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080

// Pins
#define MIC_WS   27
#define MIC_SCK  25
#define MIC_SD   18
#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

// Audio
#define SAMPLE_RATE 8000
#define AUDIO_GAIN  2.0f
#define SAMPLE_BUFFER 512
#define BATCH_SIZE 6
#define BUFFER_SIZE 8192

WebSocketsClient webSocket;
bool connected = false;

// Speaker
uint8_t* audioBuffer = nullptr;
size_t bufferSize = 0;
bool playing = false;
TaskHandle_t playTask = nullptr;

// Mic
static int32_t raw[SAMPLE_BUFFER];
#define B64_MAX_LEN (((SAMPLE_BUFFER * BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t b64Buf[B64_MAX_LEN];
static char jsonBuf[38 + B64_MAX_LEN + 3];
static int16_t batchBuffer[SAMPLE_BUFFER * BATCH_SIZE];
static int batchedSamples = 0;
int chunksSent = 0;
int messagesReceived = 0;

// ========== SPEAKER SETUP ==========
void setupSpeaker() {
  Serial.println("[SETUP] Starting speaker I2S...");
  
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
    .bck_io_num = SPK_BCLK,
    .ws_io_num = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  Serial.printf("[SETUP] Speaker driver install: %d\n", err);
  
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_start(I2S_NUM_1);
  
  Serial.println("[I2S] ✅ Speaker ready");
}

// ========== MIC SETUP ==========
void setupMicrophone() {
  Serial.println("[SETUP] Starting microphone I2S...");
  
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
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  
  esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  Serial.printf("[SETUP] Microphone driver install: %d\n", err);
  
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);
  
  Serial.println("[I2S] ✅ Microphone ready");
}

// ========== PLAYBACK TASK ==========
void playbackTask(void* param) {
  Serial.println("[TASK] Playback task started on Core 0");
  
  while (true) {
    if (playing && audioBuffer) {
      Serial.printf("[PLAY] Starting playback: %d bytes\n", bufferSize);
      
      size_t written;
      i2s_write(I2S_NUM_1, audioBuffer, bufferSize, &written, portMAX_DELAY);
      
      Serial.printf("[PLAY] Played %d bytes\n", written);
      
      playing = false;
      bufferSize = 0;
      
      Serial.println("[PLAY] Playback done, playing = false");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ========== HANDLE AUDIO ==========
void handleAudio(JsonDocument& doc) {
  Serial.println("[AUDIO] handleAudio() called");
  
  if (!doc.containsKey("data")) {
    Serial.println("[AUDIO] ❌ No data");
    return;
  }
  
  const char* b64 = doc["data"];
  if (!b64) {
    Serial.println("[AUDIO] ❌ b64 is null");
    return;
  }

  int chunkNum = doc.containsKey("chunk") ? (int)doc["chunk"] : 0;
  int totalChunks = doc.containsKey("total") ? (int)doc["total"] : 1;
  
  Serial.printf("[AUDIO] Chunk %d/%d, b64 length: %d\n", chunkNum + 1, totalChunks, strlen(b64));
  
  if (chunkNum == 0) {
    Serial.println("\n[AUDIO] 🎵 First chunk - allocating buffer");
    
    if (audioBuffer) {
      Serial.println("[AUDIO] Freeing old buffer");
      free(audioBuffer);
    }
    
    audioBuffer = (uint8_t*)malloc(BUFFER_SIZE);
    if (!audioBuffer) {
      Serial.println("[AUDIO] ❌ No memory!");
      return;
    }
    Serial.printf("[AUDIO] ✅ Allocated %d bytes\n", BUFFER_SIZE);
    bufferSize = 0;
  }
  
  // Decode
  size_t decoded = 0;
  int ret = mbedtls_base64_decode(audioBuffer, BUFFER_SIZE, &decoded,
                                   (const unsigned char*)b64, strlen(b64));
  
  if (ret != 0) {
    Serial.printf("[AUDIO] ❌ Decode failed: %d\n", ret);
    return;
  }
  
  bufferSize = decoded;
  Serial.printf("[AUDIO] Decoded %d bytes\n", decoded);
  
  // Play
  Serial.println("[AUDIO] Setting playing = true");
  playing = true;
  
  // Wait
  int waitCount = 0;
  while (playing) {
    vTaskDelay(pdMS_TO_TICKS(5));
    waitCount++;
    if (waitCount % 100 == 0) {
      Serial.printf("[AUDIO] Still waiting... (%d)\n", waitCount);
    }
  }
  Serial.println("[AUDIO] Wait complete, playing = false");
  
  if (chunkNum + 1 >= totalChunks) {
    Serial.println("[AUDIO] ✅ Last chunk complete!\n");
    i2s_zero_dma_buffer(I2S_NUM_1);
  }
}

// ========== SEND AUDIO ==========
void sendAudioChunk(int16_t* pcmData, int sampleCount) {
  size_t rawLen = sampleCount * sizeof(int16_t);

  size_t outputLen = 0;
  int ret = mbedtls_base64_encode(b64Buf, B64_MAX_LEN, &outputLen,
                                  (uint8_t*)pcmData, rawLen);
  if (ret != 0) {
    Serial.println("[ENCODE] ❌ Base64 failed!");
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
  chunksSent++;
}

// ========== WEBSOCKET ==========
void onMessage(char* payload, size_t length) {
  messagesReceived++;
  
  Serial.printf("\n[WS] ⬇️  Message #%d received (%d bytes)\n", messagesReceived, length);
  Serial.printf("[WS] First 200 chars: %.200s\n", payload);
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (error) {
    Serial.printf("[WS] ❌ JSON parse failed: %s\n", error.c_str());
    return;
  }
  
  Serial.println("[WS] ✅ JSON parsed");
  
  const char* type = doc["type"];
  if (!type) {
    Serial.println("[WS] ⚠️  No 'type' field");
    return;
  }
  
  Serial.printf("[WS] Message type: '%s'\n", type);
  
  if (strcmp(type, "tts") == 0) {
    Serial.println("[WS] ✅ TTS message - calling handleAudio()");
    handleAudio(doc);
  } else {
    Serial.printf("[WS] ⚠️  Unknown type: '%s'\n", type);
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("\n[WS] ❌ ===== DISCONNECTED =====");
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.println("\n[WS] ✅ ===== CONNECTED =====");
      Serial.printf("[WS] URL: %s\n", payload);
      Serial.println("[WS] Sending registration: {\"id\":\"esp32\"}");
      
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      connected = true;
      
      Serial.println("[WS] ✅ Registration sent");
      Serial.println("[WS] ESP32 is now registered as 'esp32'");
      break;

    case WStype_TEXT:
      onMessage((char*)payload, length);
      break;

    case WStype_PING:
      Serial.println("[WS] 🏓 Ping");
      break;

    case WStype_PONG:
      Serial.println("[WS] 🏓 Pong");
      break;

    case WStype_ERROR:
      Serial.println("[WS] ❌ ERROR!");
      break;

    default:
      Serial.printf("[WS] Event type: %d\n", type);
      break;
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║  ESP32 DEBUG VERSION                 ║");
  Serial.println("║  Detailed logging enabled            ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  
  setupSpeaker();
  setupMicrophone();
  
  Serial.println("\n[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
  
  Serial.printf("\n[WS] Connecting to %s:%d\n", VPS_HOST, VPS_PORT);
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  
  xTaskCreatePinnedToCore(playbackTask, "play", 4096, NULL, 1, &playTask, 0);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║  ✅ SETUP COMPLETE                   ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  Serial.printf("[MEM] Free heap: %d KB\n\n", ESP.getFreeHeap() / 1024);
}

// ========== LOOP ==========
void loop() {
  webSocket.loop();

  if (!connected) {
    delay(10);
    return;
  }

  if (playing) {
    // Debug: Show we're paused
    static unsigned long lastPausePrint = 0;
    if (millis() - lastPausePrint > 1000) {
      Serial.println("[MIC] ⏸️  Paused (playing = true)");
      lastPausePrint = millis();
    }
    delay(10);
    return;
  }

  // Read microphone
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
  
  if (err != ESP_OK) {
    static int errorCount = 0;
    if (errorCount++ % 100 == 0) {
      Serial.printf("[MIC] ❌ I2S read error: %d\n", err);
    }
    delay(10);
    return;
  }
  
  if (bytesRead == 0) {
    delay(10);
    return;
  }

  int count = bytesRead / sizeof(int32_t);

  // Convert
  int16_t samples[SAMPLE_BUFFER];
  for (int i = 0; i < count; i++) {
    int32_t s = (raw[i] >> 14) * AUDIO_GAIN;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    samples[i] = (int16_t)s;
  }

  // Batch
  memcpy(batchBuffer + batchedSamples, samples, count * sizeof(int16_t));
  batchedSamples += count;

  // Send
  if (batchedSamples >= SAMPLE_BUFFER * BATCH_SIZE) {
    Serial.printf("[MIC] 📤 Sending batch #%d (%d samples)\n", chunksSent + 1, batchedSamples);
    
    sendAudioChunk(batchBuffer, batchedSamples);
    batchedSamples = 0;

    if (chunksSent % 20 == 0) {
      Serial.printf("[MIC] 🎤 Sent %d batches total\n", chunksSent);
      Serial.printf("[MIC] 💾 Free heap: %d KB\n", ESP.getFreeHeap() / 1024);
    }
  }

  delay(10);
}
