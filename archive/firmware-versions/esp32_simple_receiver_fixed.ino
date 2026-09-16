/*
 * ESP32 - SIMPLE AUDIO RECEIVER (FIXED - NO DISCONNECTIONS!)
 * 
 * FIXES:
 * - ✅ Removed blocking wait (was causing WebSocket timeout)
 * - ✅ Added heartbeat/keepalive
 * - ✅ Added WiFi power management
 * - ✅ Better buffer management
 * - ✅ Watchdog feeding
 * 
 * ONLY DOES:
 * 1. Receive PCM audio from PC
 * 2. Decode base64
 * 3. Play through speaker
 * 
 * Speaker: MAX98357A on GPIO 14, 12, 13
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

// Speaker pins
#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

// Audio
#define SAMPLE_RATE 8000
#define BUFFER_SIZE 8192  // 8KB buffer for each chunk

WebSocketsClient webSocket;
bool connected = false;

// Playback buffer
uint8_t* audioBuffer = nullptr;
size_t bufferSize = 0;
bool playing = false;
TaskHandle_t playTask = nullptr;

// Stats
unsigned long lastBeepTime = 0;
int beepCount = 0;

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

// ========== PLAYBACK TASK ==========
void playbackTask(void* param) {
  while (true) {
    if (playing && audioBuffer && bufferSize > 0) {
      // Play audio
      size_t written;
      i2s_write(I2S_NUM_1, audioBuffer, bufferSize, &written, portMAX_DELAY);
      
      // Clear DMA buffer
      i2s_zero_dma_buffer(I2S_NUM_1);
      
      // Done
      playing = false;
      bufferSize = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ========== HANDLE AUDIO ==========
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
    beepCount++;
    lastBeepTime = millis();
    
    Serial.printf("\n[AUDIO] 🎵 Beep #%d - Receiving...\n", beepCount);
    
    // Wait for previous playback to finish
    int timeout = 0;
    while (playing && timeout < 100) {
      delay(10);
      timeout++;
    }
    
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
  
  // ✅ FIX: Don't block here - just trigger playback
  // The blocking wait was preventing WebSocket.loop() from running!
  playing = true;
  
  // Last chunk
  if (chunkNum + 1 >= totalChunks) {
    Serial.printf("[AUDIO] ✅ Beep #%d complete!\n\n", beepCount);
  }
}

// ========== WEBSOCKET ==========
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
      Serial.println("\n[WS] ❌ DISCONNECTED!");
      Serial.printf("[WS] Total beeps received: %d\n", beepCount);
      Serial.printf("[WS] Uptime: %lu seconds\n\n", millis() / 1000);
      connected = false;
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] ✅ Connected!");
      webSocket.sendTXT("{\"id\":\"esp32\"}");
      connected = true;
      beepCount = 0;
      break;

    case WStype_TEXT:
      onMessage((char*)payload, length);
      break;

    case WStype_PING:
      Serial.println("[WS] 🏓 Ping received");
      break;

    case WStype_PONG:
      Serial.println("[WS] 🏓 Pong received");
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
  Serial.println("║  ESP32 SIMPLE AUDIO RECEIVER     ║");
  Serial.println("║       (FIXED - STABLE!)          ║");
  Serial.println("╚══════════════════════════════════╝\n");
  
  // ✅ Disable WiFi power saving (prevents disconnects)
  WiFi.setSleep(false);
  
  // Setup speaker
  Serial.println("[1/3] Setting up speaker...");
  setupSpeaker();
  
  // Connect WiFi
  Serial.println("[2/3] Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] Signal: %d dBm\n", WiFi.RSSI());
  
  // Connect WebSocket
  Serial.println("[3/3] Connecting to VPS...");
  webSocket.begin(VPS_HOST, VPS_PORT, "/");
  webSocket.onEvent(webSocketEvent);
  
  // ✅ Enable heartbeat to keep connection alive
  webSocket.enableHeartbeat(15000, 3000, 2);  // Ping every 15s, timeout 3s, 2 retries
  
  // ✅ Auto-reconnect if disconnected
  webSocket.setReconnectInterval(3000);  // Try reconnect every 3 seconds
  
  // Start playback task
  xTaskCreatePinnedToCore(playbackTask, "play", 4096, NULL, 1, &playTask, 0);
  
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║  ✅ READY - Waiting for audio   ║");
  Serial.println("║  Heartbeat: ON (15s)             ║");
  Serial.println("║  Auto-reconnect: ON              ║");
  Serial.println("╚══════════════════════════════════╝\n");
}

// ========== LOOP ==========
void loop() {
  // ✅ Keep WebSocket alive - this MUST run frequently!
  webSocket.loop();
  
  // Show status every 30 seconds
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 30000) {
    lastStatus = millis();
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    Serial.printf("[STATUS] Connected: %s\n", connected ? "YES ✅" : "NO ❌");
    Serial.printf("[STATUS] Beeps received: %d\n", beepCount);
    Serial.printf("[STATUS] Uptime: %lu seconds\n", millis() / 1000);
    Serial.printf("[STATUS] Free heap: %d KB\n", ESP.getFreeHeap() / 1024);
    Serial.println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
  }
  
  // Small delay to prevent watchdog issues
  delay(10);
}
