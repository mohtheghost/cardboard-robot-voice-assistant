/*
 * ESP32 Voice Assistant with TTS Playback
 * 
 * Microphone: I2S_NUM_0 (GPIO 27, 25, 18)
 * Speaker:    I2S_NUM_1 (GPIO 14, 12, 13)
 * 
 * INSTALL THESE LIBRARIES FIRST:
 * 1. WebSockets by Markus Sattler
 * 2. ArduinoJson by Benoit Blanchon (VERSION 6.x - NOT v7!)
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ================= CONFIGURATION =================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define VPS_HOST  "YOUR_SERVER_IP"
#define VPS_PORT  8080

// Microphone pins (INMP441)
#define MIC_WS   27
#define MIC_SCK  25
#define MIC_SD   18

// Speaker pins (MAX98357A)
#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

#define SAMPLE_RATE 8000
#define AUDIO_GAIN  2.0f

// ================= GLOBAL VARIABLES =================
WebSocketsClient webSocket;
bool wsConnected = false;
bool registered = false;

// Microphone recording
QueueHandle_t sendQueue;
struct AudioChunk { int16_t* data; int count; };
static int32_t micBuffer[512];
static int16_t batchBuffer[512 * 6];
static int batchedSamples = 0;

// TTS playback
static uint8_t* ttsBuffer = nullptr;
static size_t ttsBufferSize = 0;
static size_t ttsBufferUsed = 0;
static int ttsTotalChunks = 0;
static int ttsReceivedChunks = 0;
static bool ttsReceiving = false;
static bool ttsReadyToPlay = false;

// Stats
static int messagesReceived = 0;

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
    .use_apll = true,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_pin_config_t pins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);
  
  Serial.println("[I2S] Microphone initialized (I2S_0)");
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
    .bck_io_num = SPK_BCLK,
    .ws_io_num = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_start(I2S_NUM_1);
  
  Serial.println("[I2S] Speaker initialized (I2S_1)");
}

// ================= TTS AUDIO HANDLING =================
void processTtsChunk(int chunkNum, int totalChunks, const char* base64Data) {
  Serial.printf("[TTS] Chunk %d/%d received\n", chunkNum + 1, totalChunks);
  
  // First chunk - allocate buffer
  if (chunkNum == 0) {
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║     🎵 RECEIVING AUDIO FROM PC        ║");
    Serial.println("╚════════════════════════════════════════╝");
    
    if (ttsBuffer) {
      free(ttsBuffer);
    }
    
    ttsBuffer = (uint8_t*)malloc(300 * 1024); // 300KB max
    if (!ttsBuffer) {
      Serial.println("[TTS] ❌ Out of memory!");
      return;
    }
    
    ttsBufferSize = 300 * 1024;
    ttsBufferUsed = 0;
    ttsTotalChunks = totalChunks;
    ttsReceivedChunks = 0;
    ttsReceiving = true;
    ttsReadyToPlay = false;
    
    Serial.printf("[TTS] Buffer allocated: %d KB\n", ttsBufferSize / 1024);
  }
  
  // Decode base64
  size_t base64Len = strlen(base64Data);
  size_t decodedLen = 0;
  
  int result = mbedtls_base64_decode(
    ttsBuffer + ttsBufferUsed,
    ttsBufferSize - ttsBufferUsed,
    &decodedLen,
    (const unsigned char*)base64Data,
    base64Len
  );
  
  if (result != 0) {
    Serial.printf("[TTS] ❌ Base64 decode failed (error %d)\n", result);
    return;
  }
  
  ttsBufferUsed += decodedLen;
  ttsReceivedChunks++;
  
  Serial.printf("[TTS] Decoded %d bytes (total: %d KB)\n", 
                decodedLen, ttsBufferUsed / 1024);
  
  // All chunks received?
  if (ttsReceivedChunks >= ttsTotalChunks) {
    ttsReceiving = false;
    ttsReadyToPlay = true;
    
    Serial.println("\n╔════════════════════════════════════════╗");
    Serial.println("║      ✅ AUDIO RECEIVED COMPLETE       ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("[TTS] Total size: %d bytes (%.1f KB)\n", 
                  ttsBufferUsed, ttsBufferUsed / 1024.0);
    Serial.printf("[TTS] Duration: %.1f seconds\n", 
                  ttsBufferUsed / 2.0 / SAMPLE_RATE);
  }
}

void playTtsAudio() {
  if (!ttsReadyToPlay || !ttsBuffer) {
    return;
  }
  
  Serial.println("\n[TTS] 🔊 Playing audio...");
  
  size_t totalBytes = ttsBufferUsed;
  size_t written = 0;
  
  while (written < totalBytes) {
    size_t toWrite = min((size_t)2048, totalBytes - written);
    size_t bytesWritten = 0;
    
    esp_err_t err = i2s_write(
      I2S_NUM_1,
      ttsBuffer + written,
      toWrite,
      &bytesWritten,
      portMAX_DELAY
    );
    
    if (err != ESP_OK) {
      Serial.printf("[TTS] ❌ I2S write error: %d\n", err);
      break;
    }
    
    written += bytesWritten;
    
    // Progress
    int percent = (written * 100) / totalBytes;
    static int lastPercent = -1;
    if (percent != lastPercent && percent % 25 == 0) {
      Serial.printf("[TTS] 🔊 %d%%\n", percent);
      lastPercent = percent;
    }
  }
  
  i2s_zero_dma_buffer(I2S_NUM_1);
  
  Serial.println("[TTS] ✅ Playback complete!\n");
  
  // Cleanup
  free(ttsBuffer);
  ttsBuffer = nullptr;
  ttsReadyToPlay = false;
}

// ================= WEBSOCKET HANDLING =================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      wsConnected = false;
      registered = false;
      break;
      
    case WStype_CONNECTED:
      Serial.println("[WS] ✅ Connected to VPS!");
      wsConnected = true;
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      registered = true;
      break;
      
    case WStype_TEXT: {
      messagesReceived++;
      
      Serial.printf("\n[WS] ⬇️  Message #%d (%d bytes)\n", messagesReceived, length);
      
      // Parse JSON
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, payload, length);
      
      if (error) {
        Serial.printf("[JSON] ❌ Parse failed: %s\n", error.c_str());
        Serial.printf("[JSON] First 200 chars: %.200s\n", (char*)payload);
        return;
      }
      
      Serial.println("[JSON] ✅ Parse OK");
      
      // Check message type
      const char* type = doc["type"];
      if (!type) {
        Serial.println("[JSON] ⚠️  No 'type' field");
        return;
      }
      
      Serial.printf("[JSON] Type: %s\n", type);
      
      // Handle TTS
      if (strcmp(type, "tts") == 0) {
        int chunkNum = doc["chunk"];
        int totalChunks = doc["total"];
        const char* data = doc["data"];
        
        if (data && strlen(data) > 0) {
          processTtsChunk(chunkNum, totalChunks, data);
        } else {
          Serial.println("[TTS] ❌ No data field");
        }
      }
      
      break;
    }
      
    case WStype_ERROR:
      Serial.println("[WS] ❌ Error!");
      break;
      
    default:
      break;
  }
}

// ================= MICROPHONE TASKS =================
void sendTask(void* param) {
  AudioChunk chunk;
  
  while (true) {
    if (xQueueReceive(sendQueue, &chunk, portMAX_DELAY)) {
      if (registered) {
        memcpy(batchBuffer + batchedSamples, chunk.data, 
               chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
        if (batchedSamples >= 512 * 6) {
          // Encode and send (simplified - keeping your existing logic)
          uint8_t b64[8192];
          size_t outLen = 0;
          
          mbedtls_base64_encode(b64, sizeof(b64), &outLen,
                               (uint8_t*)batchBuffer, 
                               batchedSamples * sizeof(int16_t));
          
          char json[16384];
          snprintf(json, sizeof(json),
                   "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"%s\"}",
                   b64);
          
          webSocket.sendTXT(json);
          batchedSamples = 0;
        }
      }
      free(chunk.data);
    }
  }
}

void playbackTask(void* param) {
  while (true) {
    if (ttsReadyToPlay) {
      playTtsAudio();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  ESP32 Voice Assistant with Playback  ║");
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
  sendQueue = xQueueCreate(64, sizeof(AudioChunk));
  
  // Tasks
  xTaskCreatePinnedToCore(sendTask, "sendTask", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(playbackTask, "playTask", 8192, NULL, 2, NULL, 0);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║        ✅ SYSTEM READY                ║");
  Serial.println("║   Waiting for audio from PC...        ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ================= LOOP (Microphone) =================
void loop() {
  webSocket.loop();
  
  if (!registered) {
    delay(10);
    return;
  }
  
  // Read from microphone
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, micBuffer, sizeof(micBuffer), 
                           &bytesRead, pdMS_TO_TICKS(100));
  
  if (err != ESP_OK || bytesRead == 0) {
    return;
  }
  
  int count = bytesRead / sizeof(int32_t);
  int16_t* pcm = (int16_t*)malloc(count * sizeof(int16_t));
  
  if (!pcm) {
    return;
  }
  
  // Convert 32-bit to 16-bit
  for (int i = 0; i < count; i++) {
    int32_t s = micBuffer[i] >> 14;
    s = s * AUDIO_GAIN;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }
  
  AudioChunk chunk = { pcm, count };
  
  if (xQueueSend(sendQueue, &chunk, 0) != pdTRUE) {
    free(pcm);
  }
  
  // Stats every 10 seconds
  static unsigned long lastStats = 0;
  if (millis() - lastStats > 10000) {
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("[STATS] Messages from VPS: %d\n", messagesReceived);
    Serial.printf("[STATS] Queue: %d/64\n", uxQueueMessagesWaiting(sendQueue));
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    lastStats = millis();
  }
}
