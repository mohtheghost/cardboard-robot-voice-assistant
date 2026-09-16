/*
 * ESP32 COMPLETE VOICE ASSISTANT
 * 
 * EXACT COMBINATION OF TWO WORKING CODES:
 * - Speaker code: 100% from working simple receiver
 * - Mic code: Added from working dual I2S code
 * 
 * I2S_NUM_0 → INMP441 Microphone (GPIO 27, 25, 18)
 * I2S_NUM_1 → MAX98357A Amplifier (GPIO 14, 12, 13)
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ================= WiFi & VPS =================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080

// ================= I2S PINS =================
// Microphone (INMP441)
#define I2S_MIC_WS   27
#define I2S_MIC_SCK  25
#define I2S_MIC_SD   18

// Speaker (MAX98357A)
#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

// ================= AUDIO CONFIG =================
#define SAMPLE_RATE 8000
#define BUFFER_SIZE 8192  // Speaker buffer (from working code)
#define AUDIO_GAIN  2.0f  // Microphone gain

// Microphone config (from working mic code)
#define DMA_BUF_LEN    512
#define DMA_BUF_COUNT  4
#define SAMPLE_BUFFER  DMA_BUF_LEN
#define SEND_QUEUE_SIZE 32
#define BATCH_SIZE 6

// ================= GLOBALS =================
WebSocketsClient webSocket;
bool connected = false;

// ========== SPEAKER GLOBALS (FROM WORKING SIMPLE RECEIVER) ==========
uint8_t* audioBuffer = nullptr;
size_t bufferSize = 0;
bool playing = false;
TaskHandle_t playTask = nullptr;

// ========== MICROPHONE GLOBALS (FROM WORKING DUAL I2S) ==========
QueueHandle_t sendQueue;
struct AudioChunk {
  int16_t* data;
  int      count;
};

static int32_t  raw[SAMPLE_BUFFER];
#define B64_MAX_LEN (((SAMPLE_BUFFER * BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t  b64Buf[B64_MAX_LEN];
static char     jsonBuf[38 + B64_MAX_LEN + 3];
static int16_t  batchBuffer[SAMPLE_BUFFER * BATCH_SIZE];
static int      batchedSamples = 0;
static int sentChunks = 0;
static int droppedChunks = 0;

// ========== SPEAKER SETUP (100% FROM WORKING SIMPLE RECEIVER) ==========
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

// ========== MICROPHONE SETUP (FROM WORKING DUAL I2S) ==========
void setupMicrophone() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUF_COUNT,
    .dma_buf_len = DMA_BUF_LEN,
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
  
  Serial.println("[I2S] ✅ Microphone ready");
}

// ========== PLAYBACK TASK (100% FROM WORKING SIMPLE RECEIVER) ==========
void playbackTask(void* param) {
  while (true) {
    if (playing && audioBuffer) {
      // Play audio
      size_t written;
      i2s_write(I2S_NUM_1, audioBuffer, bufferSize, &written, portMAX_DELAY);
      
      // Done
      playing = false;
      bufferSize = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ========== HANDLE AUDIO (100% FROM WORKING SIMPLE RECEIVER) ==========
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

// ========== WEBSOCKET (100% FROM WORKING SIMPLE RECEIVER) ==========
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
      Serial.println("[MIC] 🎤 Now recording...\n");
      break;

    case WStype_TEXT:
      onMessage((char*)payload, length);
      break;

    default:
      break;
  }
}

// ========== MICROPHONE SENDING (FROM WORKING DUAL I2S) ==========
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

void sendTask(void* param) {
  AudioChunk chunk;
  
  while (true) {
    if (xQueueReceive(sendQueue, &chunk, portMAX_DELAY)) {
      
      if (connected && !playing) {  // ✅ Don't send during playback
        memcpy(batchBuffer + batchedSamples, chunk.data, chunk.count * sizeof(int16_t));
        batchedSamples += chunk.count;
        
        if (batchedSamples >= SAMPLE_BUFFER * BATCH_SIZE) {
          sendAudioChunk(batchBuffer, batchedSamples);
          batchedSamples = 0;
          
          // Show activity every 20 sends
          if (sentChunks % 20 == 0) {
            Serial.printf("[MIC] 🎤 Sent %d batches\n", sentChunks);
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
  Serial.println("║  EXACT COMBO OF WORKING CODES       ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  
  // Setup BOTH I2S
  Serial.println("[1/4] Setting up speaker...");
  setupSpeaker();
  
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
  
  // Create microphone queue
  sendQueue = xQueueCreate(SEND_QUEUE_SIZE, sizeof(AudioChunk));
  if (sendQueue == NULL) {
    Serial.println("[ERROR] Failed to create queue!");
    while (1) delay(1000);
  }
  
  // Start tasks
  xTaskCreatePinnedToCore(playbackTask, "play", 4096, NULL, 1, &playTask, 0);
  xTaskCreatePinnedToCore(sendTask, "send", 8192, NULL, 1, NULL, 0);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║  ✅ ALL SYSTEMS READY!              ║");
  Serial.println("║                                      ║");
  Serial.println("║  🎤 Microphone: Recording           ║");
  Serial.println("║  🔊 Speaker: Ready for playback     ║");
  Serial.println("╚══════════════════════════════════════╝\n");
  
  Serial.printf("[MEM] Free heap: %d KB\n\n", ESP.getFreeHeap() / 1024);
}

// ========== LOOP ==========
void loop() {
  // WebSocket must run
  webSocket.loop();

  if (!connected) {
    delay(10);
    return;
  }

  // ✅ Pause mic during TTS playback
  if (playing) {
    delay(10);
    return;
  }

  // Read microphone (FROM WORKING DUAL I2S CODE)
  size_t bytes_read = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes_read, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytes_read == 0) return;

  int count = bytes_read / sizeof(int32_t);

  int16_t* pcm = (int16_t*)malloc(count * sizeof(int16_t));
  if (!pcm) {
    droppedChunks++;
    return;
  }

  // Convert 32-bit to 16-bit with gain
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
  
  delay(10);
}
