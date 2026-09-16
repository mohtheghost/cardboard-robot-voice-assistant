/*
 * ESP32 COMPLETE VOICE ASSISTANT - FIXED
 * 
 * FIX: Made 'playing' variable volatile for multi-core synchronization
 * This fixes microphone not recording issue!
 * 
 * FEATURES:
 * ✅ Record from microphone → Send to PC
 * ✅ Receive TTS from PC → Play through speaker
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
// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// VPS
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080

// I2S Pins - Microphone (INMP441)
#define MIC_WS   27
#define MIC_SCK  25
#define MIC_SD   18

// I2S Pins - Speaker (MAX98357A)
#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

// Audio
#define SAMPLE_RATE 8000
#define AUDIO_GAIN  2.0f

// Microphone
#define MIC_BUFFER_SIZE 512
#define SEND_QUEUE_SIZE 16
#define BATCH_SIZE      4

// Speaker
#define BUFFER_SIZE 8192  // 8KB buffer for each chunk

// ================= GLOBALS =================
WebSocketsClient webSocket;
bool connected = false;

// Microphone
QueueHandle_t sendQueue;
struct AudioChunk { int16_t* data; int count; };
static int32_t micRaw[MIC_BUFFER_SIZE];
static int16_t batchBuffer[MIC_BUFFER_SIZE * BATCH_SIZE];
static int batchedSamples = 0;

// Speaker playback buffer
uint8_t* audioBuffer = nullptr;
size_t bufferSize = 0;
volatile bool playing = false;  // ✅ FIXED: volatile for multi-core access!
TaskHandle_t playTask = nullptr;

// Stats
int micChunksSent = 0;

// ========== MICROPHONE SETUP ==========
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
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);
  
  Serial.println("[I2S] ✅ Microphone ready");
}

// ========== SPEAKER SETUP ==========
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
  
  Serial.println("[I2S] ✅ Speaker ready");
}

// ========== PLAYBACK TASK (UNCHANGED FROM WORKING CODE) ==========
void playbackTask(void* param) {
  while (true) {
    if (playing && audioBuffer) {
      Serial.println("[PLAY] 🔊 Playing audio...");
      
      // Play audio
      size_t written;
      i2s_write(I2S_NUM_1, audioBuffer, bufferSize, &written, portMAX_DELAY);
      
      // Done
      playing = false;  // ✅ This will now be properly seen by other cores
      bufferSize = 0;
      
      Serial.println("[PLAY] ✅ Playback complete");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ========== HANDLE AUDIO (UNCHANGED FROM WORKING CODE) ==========
void handleAudio(JsonDocument& doc) {
  // Get data
  if (!doc.containsKey("data")) {
    Serial.println("[AUDIO] ❌ No data");
    return;
  }
  
  const char* b64 = doc["data"];
  if (!b64) return;
  
  int chunkNum = doc.containsKey("chunk") ? (int)doc["chunk"] : 0;
  int totalChunks = doc.containsKey("total") ? (int)doc["total"] : 1;
  
  // First chunk - allocate buffer
  if (chunkNum == 0) {
    Serial.println("\n[AUDIO] 🎵 Receiving...");
    if (audioBuffer) free(audioBuffer);
    audioBuffer = (uint8_t*)malloc(BUFFER_SIZE);
    if (!audioBuffer) {
      Serial.println("[AUDIO] ❌ No memory!");
      return;
    }
    bufferSize = 0;
  }
  
  // Decode base64
  size_t decoded = 0;
  int ret = mbedtls_base64_decode(
    audioBuffer,
    BUFFER_SIZE,
    &decoded,
    (const unsigned char*)b64,
    strlen(b64)
  );
  
  if (ret != 0) {
    Serial.printf("[AUDIO] ❌ Decode failed: %d\n", ret);
    return;
  }
  
  bufferSize = decoded;
  
  // Progress
  Serial.printf("[AUDIO] Chunk %d/%d (%d bytes)\n", chunkNum + 1, totalChunks, decoded);
  
  // Play immediately
  playing = true;
  
  // Wait for playback
  while (playing) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  
  // Last chunk - complete
  if (chunkNum + 1 >= totalChunks) {
    Serial.println("[AUDIO] ✅ Done!\n");
    i2s_zero_dma_buffer(I2S_NUM_1);
  }
}

// ========== WEBSOCKET (UNCHANGED FROM WORKING CODE) ==========
void onMessage(char* payload, size_t length) {
  // Parse JSON
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[WS] ❌ Bad JSON");
    return;
  }
  
  // Check type
  const char* type = doc["type"];
  if (!type) return;
  
  if (strcmp(type, "tts") == 0) {
    handleAudio(doc);
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] ❌ Disconnected");
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] ✅ Connected!");
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      connected = true;
      Serial.println("\n╔══════════════════════════════════════╗");
      Serial.println("║  🎤 MICROPHONE NOW ACTIVE           ║");
      Serial.println("║  Speak to test!                     ║");
      Serial.println("╚══════════════════════════════════════╝\n");
      break;

    case WStype_TEXT:
      onMessage((char*)payload, length);
      break;

    default:
      break;
  }
}

// ========== MICROPHONE SENDING (NEW) ==========
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
  
  micChunksSent++;
}

void sendTask(void* param) {
  AudioChunk chunk;
  
  while (true) {
    if (xQueueReceive(sendQueue, &chunk, portMAX_DELAY)) {
      if (connected && !playing) {  // Don't send during TTS playback
        memcpy(batchBuffer + batchedSamples, chunk.data, chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
        if (batchedSamples >= MIC_BUFFER_SIZE * BATCH_SIZE) {
          sendAudioChunk(batchBuffer, batchedSamples);
          batchedSamples = 0;
          
          // Show microphone activity every 10 sends
          if (micChunksSent % 10 == 0) {
            Serial.printf("[MIC] 🎤 Sent %d chunks to PC\n", micChunksSent);
          }
        }
      }
      free(chunk.data);
    }
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║  ESP32 COMPLETE VOICE ASSISTANT     ║");
  Serial.println("║  🎤 Microphone + 🔊 Speaker         ║");
  Serial.println("║  ✅ FIXED - volatile playing var    ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  
  // Setup speaker
  Serial.println("[1/4] Setting up speaker...");
  setupSpeaker();
  
  // Setup microphone
  Serial.println("[2/4] Setting up microphone...");
  setupMicrophone();
  
  // Connect WiFi
  Serial.println("[3/4] Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
  
  // Connect WebSocket
  Serial.println("[4/4] Connecting to VPS...");
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  
  // Create queue for microphone
  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));
  if (sendQueue == NULL) {
    Serial.println("[ERROR] Failed to create queue!");
    while (1) delay(1000);
  }
  
  // Start tasks
  xTaskCreatePinnedToCore(playbackTask, "play", 4096, NULL, 1, &playTask, 0);
  xTaskCreatePinnedToCore(sendTask, "send", 6144, NULL, 1, NULL, 0);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║  ✅ ALL SYSTEMS READY!              ║");
  Serial.println("║                                      ║");
  Serial.println("║  🎤 Recording from microphone       ║");
  Serial.println("║  🔊 Ready for TTS playback          ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  
  Serial.printf("[MEM] Free heap: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.println();
}

// ========== LOOP ==========
void loop() {
  webSocket.loop();

  if (!connected) {
    delay(10);
    return;
  }

  // ✅ Pause microphone during playback (volatile ensures this works correctly)
  if (playing) {
    delay(10);
    return;
  }

  // Read microphone
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
  }
  
  delay(10);
}
