/*
 * ESP32 - SIMPLE MICROPHONE TEST
 * 
 * ONLY DOES:
 * 1. Record audio from INMP441 microphone
 * 2. Encode to base64
 * 3. Send to PC
 * 
 * That's it. Nothing else.
 * 
 * Microphone: INMP441 on GPIO 27, 25, 18
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

// VPS
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080

// Microphone pins
#define MIC_WS   27
#define MIC_SCK  25
#define MIC_SD   18

// Audio
#define SAMPLE_RATE 8000
#define AUDIO_GAIN  2.0f

// Microphone config
#define SAMPLE_BUFFER 512
#define BATCH_SIZE 6

WebSocketsClient webSocket;
bool connected = false;

// Buffers
static int32_t raw[SAMPLE_BUFFER];
#define B64_MAX_LEN (((SAMPLE_BUFFER * BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t b64Buf[B64_MAX_LEN];
static char jsonBuf[38 + B64_MAX_LEN + 3];
static int16_t batchBuffer[SAMPLE_BUFFER * BATCH_SIZE];
static int batchedSamples = 0;

// Stats
int chunksSent = 0;

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

// ========== SEND AUDIO ==========
void sendAudioChunk(int16_t* pcmData, int sampleCount) {
  size_t rawLen = sampleCount * sizeof(int16_t);

  // Base64 encode
  size_t outputLen = 0;
  int ret = mbedtls_base64_encode(b64Buf, B64_MAX_LEN, &outputLen,
                                  (uint8_t*)pcmData, rawLen);
  if (ret != 0) {
    Serial.println("[ENCODE] ❌ Base64 failed!");
    return;
  }
  b64Buf[outputLen] = '\0';

  // Create JSON
  int headerLen = snprintf(jsonBuf, sizeof(jsonBuf),
                           "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"");
  memcpy(jsonBuf + headerLen, b64Buf, outputLen);
  jsonBuf[headerLen + outputLen]     = '"';
  jsonBuf[headerLen + outputLen + 1] = '}';
  jsonBuf[headerLen + outputLen + 2] = '\0';

  // Send
  webSocket.sendTXT(jsonBuf);
  chunksSent++;
}

// ========== WEBSOCKET ==========
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
      Serial.println("\n╔══════════════════════════════════╗");
      Serial.println("║  🎤 MICROPHONE NOW RECORDING    ║");
      Serial.println("║  Speak to test!                 ║");
      Serial.println("╚══════════════════════════════════╝\n");
      break;

    default:
      break;
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  ESP32 SIMPLE MIC TEST           ║");
  Serial.println("╚══════════════════════════════════╝\n");
  
  // Setup microphone
  Serial.println("[1/3] Setting up microphone...");
  setupMicrophone();
  
  // Connect WiFi
  Serial.println("[2/3] Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
  
  // Connect WebSocket
  Serial.println("[3/3] Connecting to VPS...");
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
  
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  ✅ READY - Recording audio     ║");
  Serial.println("╚══════════════════════════════════╝\n");
}

// ========== LOOP ==========
void loop() {
  webSocket.loop();

  if (!connected) {
    delay(10);
    return;
  }

  // Read microphone
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytesRead, pdMS_TO_TICKS(100));
  
  if (err != ESP_OK) {
    Serial.printf("[MIC] ❌ I2S read error: %d\n", err);
    delay(10);
    return;
  }
  
  if (bytesRead == 0) {
    delay(10);
    return;
  }

  int count = bytesRead / sizeof(int32_t);

  // Convert 32-bit to 16-bit with gain
  int16_t samples[SAMPLE_BUFFER];
  for (int i = 0; i < count; i++) {
    int32_t s = (raw[i] >> 14) * AUDIO_GAIN;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    samples[i] = (int16_t)s;
  }

  // Batch samples
  memcpy(batchBuffer + batchedSamples, samples, count * sizeof(int16_t));
  batchedSamples += count;

  // Send when batch is full
  if (batchedSamples >= SAMPLE_BUFFER * BATCH_SIZE) {
    sendAudioChunk(batchBuffer, batchedSamples);
    batchedSamples = 0;

    // Show activity every 20 sends
    if (chunksSent % 20 == 0) {
      Serial.printf("[MIC] 🎤 Sent %d batches to PC\n", chunksSent);
    }
  }

  delay(10);
}
