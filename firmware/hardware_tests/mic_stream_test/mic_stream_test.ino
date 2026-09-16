/*
 * ESP32 - MICROPHONE TEST
 *
 * Only does three things:
 *   1. records audio from the INMP441 microphone (I2S port 0)
 *   2. base64-encodes it
 *   3. sends it to the robot server over the WebSocket
 *
 * Nothing is played back. Start the server, flash this, then talk near the mic:
 * the server console shows a live level bar ([VAD] ... | RMS: ... | ████|) that
 * must jump when you speak. If it stays flat, check the mic wiring
 * (SCK GPIO25, WS GPIO27, SD GPIO18, L/R -> GND, VDD 3V3).
 *
 * Copy secrets.h.example to secrets.h (same folder) and fill in your values.
 * Library: "WebSockets" by Markus Sattler.
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, VPS_HOST, VPS_PORT, ROBOT_AUTH_TOKEN

// Microphone pins (INMP441)
#define MIC_WS   27
#define MIC_SCK  25
#define MIC_SD   18

// Audio - must match Config.SampleRate in the server
#define SAMPLE_RATE 16000
#define AUDIO_GAIN  1.5f

// Microphone config
#define SAMPLE_BUFFER 512
#define BATCH_SIZE 3

WebSocketsClient webSocket;
bool connected = false;

// Buffers
static int32_t raw[SAMPLE_BUFFER];
#define B64_MAX_LEN (((SAMPLE_BUFFER * BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
#define JSON_HEADER "{\"type\":\"audio\",\"data\":\""
static uint8_t b64Buf[B64_MAX_LEN];
static char jsonBuf[(sizeof(JSON_HEADER) - 1) + B64_MAX_LEN + 3];
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
  pins.mck_io_num = I2S_PIN_NO_CHANGE;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);

  Serial.printf("[I2S] ✅ Microphone ready (%d Hz)\n", SAMPLE_RATE);
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

  // Create JSON: {"type":"audio","data":"<base64>"}
  int headerLen = snprintf(jsonBuf, sizeof(jsonBuf), JSON_HEADER);
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

    case WStype_CONNECTED: {
      Serial.println("[WS] ✅ Connected!");
      char reg[128];
      if (strlen(ROBOT_AUTH_TOKEN) > 0)
        snprintf(reg, sizeof(reg), "{\"id\":\"esp32\",\"token\":\"%s\"}", ROBOT_AUTH_TOKEN);
      else
        snprintf(reg, sizeof(reg), "{\"id\":\"esp32\"}");
      webSocket.sendTXT(reg);
      connected = true;
      Serial.println("\n╔══════════════════════════════════╗");
      Serial.println("║  🎤 MICROPHONE NOW STREAMING    ║");
      Serial.println("║  Speak and watch the server!    ║");
      Serial.println("╚══════════════════════════════════╝\n");
      break;
    }

    default:
      break;
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  ESP32 MICROPHONE TEST           ║");
  Serial.println("╚══════════════════════════════════╝\n");

  Serial.println("[1/3] Setting up microphone...");
  setupMicrophone();

  Serial.println("[2/3] Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());

  Serial.printf("[3/3] Connecting to server %s:%d ...\n", VPS_HOST, VPS_PORT);
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
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

  // Convert 32-bit I2S frames (24-bit data) to 16-bit PCM with gain
  int16_t samples[SAMPLE_BUFFER];
  for (int i = 0; i < count; i++) {
    int32_t s = (int32_t)((raw[i] >> 14) * AUDIO_GAIN);
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
      Serial.printf("[MIC] 🎤 Sent %d batches to the server\n", chunksSent);
    }
  }
}
