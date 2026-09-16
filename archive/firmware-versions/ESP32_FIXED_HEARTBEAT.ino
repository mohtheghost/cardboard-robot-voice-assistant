/*
 * ESP32 COMPLETE VOICE ASSISTANT - STABLE WS + COMPLETE TTS
 * ✅ FIXED: Heartbeat enabled to maintain server connection
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ======================= CONFIG =======================

// LED
#define LED_PIN 2

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_DELAY_MS 500

// VPS / WebSocket
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
#define VPS_PATH "/"
#define WS_RECONNECT_INTERVAL_MS 500

// ✅ FIXED: Heartbeat ENABLED (prevents "ESP32 not connected" error)
#define WS_HEARTBEAT_INTERVAL_MS 15000  // Ping every 15 seconds
#define WS_HEARTBEAT_TIMEOUT_MS 30000   // Wait 30 seconds for pong
#define WS_HEARTBEAT_PONG_RETRIES 3     // Retry 3 times

// I2S Mic (INMP441) - I2S_NUM_0
#define I2S_MIC_SERIAL_CLOCK  25
#define I2S_MIC_WORD_SELECT   27
#define I2S_MIC_SERIAL_DATA   18

// I2S Spk (MAX98357A) - I2S_NUM_1
#define I2S_SPK_SERIAL_CLOCK  12
#define I2S_SPK_WORD_SELECT   14
#define I2S_SPK_SERIAL_DATA   13

// Audio
#define AUDIO_SAMPLE_RATE     8000
#define MIC_I2S_BITS          32
#define SPK_I2S_BITS          16
#define MIC_GAIN_MULTIPLIER   2.0f
#define SPK_VOLUME_GAIN       2.0f

// I2S DMA
#define I2S_DMA_BUF_COUNT     4
#define I2S_DMA_BUF_LEN       512

// Microphone
#define MIC_READ_BUFFER_SIZE  512
#define MIC_BATCH_SIZE        6
#define MIC_SEND_QUEUE_SIZE   32

// Speaker
#define SPK_WRITE_CHUNK_SIZE  2048
#define SPK_PLAY_QUEUE_SIZE   12   // queue depth (we block instead of drop)

// WebSocket send queue (so ONLY loop() touches webSocket.sendTXT)
#define WS_SEND_QUEUE_SIZE    16

// Tasks
#define TASK_MIC_SEND_STACK_SIZE      8192
#define TASK_MIC_SEND_PRIORITY        1
#define TASK_MIC_SEND_CORE            0

#define TASK_SPEAKER_PLAY_STACK_SIZE  4096
#define TASK_SPEAKER_PLAY_PRIORITY    2
#define TASK_SPEAKER_PLAY_CORE        0

// Debug
#define DEBUG_WEBSOCKET  true   // ✅ Enable to see heartbeat pings
#define DEBUG_MICROPHONE false
#define DEBUG_SPEAKER    false

// ======================= TYPES =======================

struct MicrophoneChunk {
  int16_t* samples;
  size_t sampleCount;
};

struct SpeakerState {
  volatile bool isPlaying;
};

struct SpeakerChunk {
  uint8_t* data;
  size_t length;
  uint32_t streamId;
  int chunkNumber;
  int totalChunks;
};

struct WsMessage {
  char* data;
};

// ======================= GLOBALS =======================

WebSocketsClient webSocket;
volatile bool wsConnected = false;
volatile bool wsRegistered = false;

// Mic buffers
QueueHandle_t micSendQueue;
static int32_t micRawBuffer[MIC_READ_BUFFER_SIZE];
static int16_t micBatchBuffer[MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE];
static size_t micBatchedSamples = 0;

#define MIC_B64_MAX_LEN (((MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t micBase64Buffer[MIC_B64_MAX_LEN];
static char micJsonBuffer[38 + MIC_B64_MAX_LEN + 3];

// Speaker
static SpeakerState speaker = { .isPlaying = false };
QueueHandle_t spkPlayQueue;
TaskHandle_t speakerPlaybackTask = nullptr;

// WS send queue
QueueHandle_t wsSendQueue;

// TTS stream tracking
static volatile uint32_t g_ttsStreamId = 0;

// ======================= UTILS =======================

void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW); }

void ledBlink(int times, int delayMs = 100) {
  for (int i = 0; i < times; i++) {
    ledOn();  delay(delayMs);
    ledOff(); delay(delayMs);
  }
}

String formatBytes(size_t bytes) {
  if (bytes < 1024) return String(bytes) + " B";
  if (bytes < 1024 * 1024) return String(bytes / 1024.0, 2) + " KB";
  return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

static inline size_t base64MaxDecodedLen(size_t base64Len, const char* s) {
  size_t out = (base64Len / 4) * 3;
  size_t pad = 0;
  if (base64Len >= 1 && s[base64Len - 1] == '=') pad++;
  if (base64Len >= 2 && s[base64Len - 2] == '=') pad++;
  if (out >= pad) out -= pad;
  return out;
}

void flushSpeakerQueue() {
  if (!spkPlayQueue) return;
  SpeakerChunk c;
  while (xQueueReceive(spkPlayQueue, &c, 0) == pdTRUE) {
    free(c.data);
  }
}

// ======================= I2S INIT =======================

bool initializeMicrophone() {
  Serial.println("[MIC] Initializing INMP441 (I2S_NUM_0) ...");

  i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)MIC_I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_MIC_SERIAL_CLOCK,
    .ws_io_num = I2S_MIC_WORD_SELECT,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_SERIAL_DATA
  };
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t r = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_driver_install failed: %d\n", r); return false; }

  r = i2s_set_pin(I2S_NUM_0, &pinConfig);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_set_pin failed: %d\n", r); return false; }

  r = i2s_start(I2S_NUM_0);
  if (r != ESP_OK) { Serial.printf("[MIC] i2s_start failed: %d\n", r); return false; }

  Serial.println("[MIC] ✅ Microphone ready");
  return true;
}

bool initializeSpeaker() {
  Serial.println("[SPK] Initializing MAX98357A (I2S_NUM_1) ...");

  i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SPK_I2S_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = I2S_DMA_BUF_COUNT,
    .dma_buf_len = I2S_DMA_BUF_LEN,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_SPK_SERIAL_CLOCK,
    .ws_io_num = I2S_SPK_WORD_SELECT,
    .data_out_num = I2S_SPK_SERIAL_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;

  esp_err_t r = i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_driver_install failed: %d\n", r); return false; }

  r = i2s_set_pin(I2S_NUM_1, &pinConfig);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_set_pin failed: %d\n", r); return false; }

  r = i2s_start(I2S_NUM_1);
  if (r != ESP_OK) { Serial.printf("[SPK] i2s_start failed: %d\n", r); return false; }

  Serial.println("[SPK] ✅ Speaker ready");
  return true;
}

// ======================= WS SEND (single-threaded) =======================

void queueWsSend(const char* s) {
  if (!wsSendQueue) return;

  size_t len = strlen(s);
  char* buf = (char*)malloc(len + 1);
  if (!buf) return;
  memcpy(buf, s, len + 1);

  WsMessage m = { buf };
  if (xQueueSend(wsSendQueue, &m, 0) != pdTRUE) {
    free(buf);
  }
}

// ======================= MICROPHONE =======================

void sendMicrophoneChunkQueued(int16_t* samples, size_t sampleCount) {
  if (!wsConnected || !wsRegistered) return;

  size_t pcmBytes = sampleCount * sizeof(int16_t);

  size_t base64Length = 0;
  int r = mbedtls_base64_encode(
    micBase64Buffer,
    MIC_B64_MAX_LEN,
    &base64Length,
    (uint8_t*)samples,
    pcmBytes
  );
  if (r != 0) return;

  micBase64Buffer[base64Length] = '\0';

  int headerLength = snprintf(
    micJsonBuffer,
    sizeof(micJsonBuffer),
    "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\""
  );

  memcpy(micJsonBuffer + headerLength, micBase64Buffer, base64Length);
  micJsonBuffer[headerLength + base64Length] = '"';
  micJsonBuffer[headerLength + base64Length + 1] = '}';
  micJsonBuffer[headerLength + base64Length + 2] = '\0';

  queueWsSend(micJsonBuffer);

  if (DEBUG_MICROPHONE) {
    static uint32_t n = 0;
    if ((++n % 20) == 0) Serial.printf("[MIC] queued %lu\n", (unsigned long)n);
  }
}

void microphoneSendTask(void* parameter) {
  MicrophoneChunk chunk;

  while (true) {
    if (xQueueReceive(micSendQueue, &chunk, portMAX_DELAY) == pdTRUE) {
      if (wsRegistered && !speaker.isPlaying) {
        memcpy(micBatchBuffer + micBatchedSamples, chunk.samples, chunk.sampleCount * sizeof(int16_t));
        micBatchedSamples += chunk.sampleCount;

        if (micBatchedSamples >= (MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE)) {
          sendMicrophoneChunkQueued(micBatchBuffer, micBatchedSamples);
          micBatchedSamples = 0;
        }
      }
      free(chunk.samples);
    }
  }
}

// ======================= SPEAKER (NO DROPS) =======================

void handleTtsAudioChunk(JsonDocument& doc) {
  if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) return;

  int chunkNumber = doc["chunk"];
  int totalChunks = doc["total"];
  const char* base64Data = doc["data"];
  if (!base64Data) return;

  // New TTS stream
  if (chunkNumber == 0) {
    g_ttsStreamId++;
    flushSpeakerQueue();
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════════════╗");
    Serial.println("║                 TTS AUDIO RECEIVING/PLAYING                ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝");
    Serial.printf("[SPK] New stream id=%lu total=%d\n", (unsigned long)g_ttsStreamId, totalChunks);
  }

  // IMPORTANT: never drop TTS. If queue is full, wait until there is space.
  // This creates TCP backpressure and stops the sender from flooding us.
  while (uxQueueSpacesAvailable(spkPlayQueue) == 0) {
    delay(2); // yield (prevents WDT issues)
  }

  size_t base64Len = strlen(base64Data);
  size_t maxDecoded = base64MaxDecodedLen(base64Len, base64Data);
  if (maxDecoded == 0) maxDecoded = 1;

  uint8_t* decoded = (uint8_t*)malloc(maxDecoded);
  if (!decoded) {
    Serial.printf("[SPK] malloc failed (%s)\n", formatBytes(maxDecoded).c_str());
    return;
  }

  size_t decodedLen = 0;
  int r = mbedtls_base64_decode(
    decoded,
    maxDecoded,
    &decodedLen,
    (const unsigned char*)base64Data,
    base64Len
  );

  if (r != 0 || decodedLen == 0) {
    Serial.printf("[SPK] base64 decode failed r=%d\n", r);
    free(decoded);
    return;
  }

  SpeakerChunk c;
  c.data = decoded;
  c.length = decodedLen;
  c.streamId = g_ttsStreamId;
  c.chunkNumber = chunkNumber;
  c.totalChunks = totalChunks;

  // should succeed (we waited for space)
  if (xQueueSend(spkPlayQueue, &c, 0) != pdTRUE) {
    free(decoded);
    Serial.println("[SPK] ERROR: enqueue failed unexpectedly");
    return;
  }

  if ((chunkNumber + 1) % 5 == 0 || (chunkNumber + 1) == totalChunks) {
    int progress = ((chunkNumber + 1) * 100) / totalChunks;
    Serial.printf("[SPK] Progress: %d%% (%d/%d, %s decoded)\n",
                  progress, chunkNumber + 1, totalChunks, formatBytes(decodedLen).c_str());
  }
}

void speakerPlaybackTaskFunction(void* parameter) {
  SpeakerChunk c;

  while (true) {
    if (xQueueReceive(spkPlayQueue, &c, portMAX_DELAY) == pdTRUE) {

      // Skip stale chunks from older streams
      if (c.streamId != g_ttsStreamId) {
        free(c.data);
        continue;
      }

      speaker.isPlaying = true;

      // volume gain in-place
      int16_t* audioSamples = (int16_t*)c.data;
      size_t sampleCount = c.length / sizeof(int16_t);

      for (size_t i = 0; i < sampleCount; i++) {
        int32_t amplified = (int32_t)(audioSamples[i] * SPK_VOLUME_GAIN);
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        audioSamples[i] = (int16_t)amplified;
      }

      // play
      size_t totalWritten = 0;
      while (totalWritten < c.length) {
        size_t toWrite = min((size_t)SPK_WRITE_CHUNK_SIZE, c.length - totalWritten);
        size_t written = 0;

        esp_err_t r = i2s_write(I2S_NUM_1, c.data + totalWritten, toWrite, &written, portMAX_DELAY);
        if (r != ESP_OK) {
          Serial.printf("[SPK] i2s_write error: %d\n", r);
          break;
        }
        totalWritten += written;
      }

      // last chunk played -> COMPLETE
      if (c.chunkNumber + 1 >= c.totalChunks) {
        i2s_zero_dma_buffer(I2S_NUM_1);
        Serial.println();
        Serial.println("╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           ✅ TTS AUDIO PLAYBACK COMPLETE                   ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
      }

      free(c.data);
      speaker.isPlaying = false;
    }
  }
}

// ======================= WEBSOCKET =======================

void handleWebSocketMessage(char* payload, size_t length) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    if (DEBUG_WEBSOCKET) Serial.printf("[WS] JSON error: %s\n", err.c_str());
    return;
  }

  const char* type = doc["type"];
  if (!type) return;

  if (strcmp(type, "tts") == 0) {
    handleTtsAudioChunk(doc);
  }
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("\n[WS] ❌ DISCONNECTED");
      ledOn();
      wsConnected = false;
      wsRegistered = false;
      break;

    case WStype_CONNECTED:
      Serial.printf("\n[WS] ✅ CONNECTED: %s\n", payload);
      ledOff();
      wsConnected = true;

      webSocket.sendTXT("{\"id\":\"esp32\"}");
      wsRegistered = true;
      
      Serial.println("[WS] ✅ ESP32 REGISTERED - Ready to receive TTS");
      Serial.printf("[WS] 🔔 Heartbeat enabled: ping every %ds, timeout %ds\n", 
                    WS_HEARTBEAT_INTERVAL_MS/1000, WS_HEARTBEAT_TIMEOUT_MS/1000);
      break;

    case WStype_TEXT:
      handleWebSocketMessage((char*)payload, length);
      break;

    case WStype_PING:
      if (DEBUG_WEBSOCKET) Serial.println("[WS] 🏓 PING received");
      break;

    case WStype_PONG:
      if (DEBUG_WEBSOCKET) Serial.println("[WS] 🏓 PONG sent (connection alive)");
      break;

    case WStype_ERROR:
      Serial.println("[WS] ❌ WebSocket ERROR");
      ledBlink(3, 100);
      break;

    default:
      if (DEBUG_WEBSOCKET) Serial.printf("[WS] Event type: %d\n", type);
      break;
  }
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║       ESP32 VOICE ASSISTANT - HEARTBEAT ENABLED         ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝\n");

  pinMode(LED_PIN, OUTPUT);
  ledOff();
  ledBlink(2, 150);

  // I2S
  if (!initializeSpeaker()) {
    Serial.println("FATAL: speaker init failed");
    while (1) delay(1000);
  }
  if (!initializeMicrophone()) {
    Serial.println("FATAL: mic init failed");
    while (1) delay(1000);
  }

  // WiFi
  Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("FATAL: WiFi connect timeout");
      while (1) delay(1000);
    }
    delay(WIFI_RETRY_DELAY_MS);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WiFi] ✅ Connected! IP=%s RSSI=%d dBm\n", 
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // WebSocket
  Serial.printf("[WS] Connecting to %s:%d%s\n", VPS_HOST, VPS_PORT, VPS_PATH);
  webSocket.begin(VPS_HOST, VPS_PORT, VPS_PATH);
  webSocket.onEvent(onWebSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  
  // ✅ CRITICAL: Enable heartbeat
  if (WS_HEARTBEAT_INTERVAL_MS > 0) {
    webSocket.enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS, 
                              WS_HEARTBEAT_TIMEOUT_MS, 
                              WS_HEARTBEAT_PONG_RETRIES);
    Serial.printf("[WS] ✅ Heartbeat configured: %dms interval, %dms timeout\n",
                  WS_HEARTBEAT_INTERVAL_MS, WS_HEARTBEAT_TIMEOUT_MS);
  }

  ledOn(); // until WS connects

  // Queues
  micSendQueue = xQueueCreate(MIC_SEND_QUEUE_SIZE, sizeof(MicrophoneChunk));
  spkPlayQueue = xQueueCreate(SPK_PLAY_QUEUE_SIZE, sizeof(SpeakerChunk));
  wsSendQueue  = xQueueCreate(WS_SEND_QUEUE_SIZE, sizeof(WsMessage));

  if (!micSendQueue || !spkPlayQueue || !wsSendQueue) {
    Serial.println("FATAL: queue create failed");
    while (1) delay(1000);
  }

  // Tasks
  if (xTaskCreatePinnedToCore(microphoneSendTask, "MicSend", TASK_MIC_SEND_STACK_SIZE, NULL,
                             TASK_MIC_SEND_PRIORITY, NULL, TASK_MIC_SEND_CORE) != pdPASS) {
    Serial.println("FATAL: MicSend task failed");
    while (1) delay(1000);
  }

  if (xTaskCreatePinnedToCore(speakerPlaybackTaskFunction, "SpkPlay", TASK_SPEAKER_PLAY_STACK_SIZE, NULL,
                             TASK_SPEAKER_PLAY_PRIORITY, &speakerPlaybackTask, TASK_SPEAKER_PLAY_CORE) != pdPASS) {
    Serial.println("FATAL: SpkPlay task failed");
    while (1) delay(1000);
  }

  Serial.println("\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║              ✅ ALL SYSTEMS OPERATIONAL                  ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝\n");
}

void loop() {
  // keep WiFi alive
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 2000) {
      Serial.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
      lastTry = millis();
    }
  }

  // ✅ WS service (handles heartbeat automatically)
  webSocket.loop();

  // send queued WS messages (ONLY HERE)
  WsMessage m;
  while (xQueueReceive(wsSendQueue, &m, 0) == pdTRUE) {
    if (wsConnected && wsRegistered) {
      webSocket.sendTXT(m.data);
    }
    free(m.data);
  }

  if (!wsConnected) {
    delay(5);
    return;
  }

  // pause mic while playing to avoid feedback
  if (speaker.isPlaying) {
    delay(1);
    return;
  }

  // mic read
  size_t bytesRead = 0;
  esp_err_t r = i2s_read(I2S_NUM_0, micRawBuffer, sizeof(micRawBuffer), &bytesRead, pdMS_TO_TICKS(100));
  if (r != ESP_OK || bytesRead == 0) {
    delay(1);
    return;
  }

  size_t sampleCount = bytesRead / sizeof(int32_t);

  int16_t* converted = (int16_t*)malloc(sampleCount * sizeof(int16_t));
  if (!converted) {
    delay(1);
    return;
  }

  for (size_t i = 0; i < sampleCount; i++) {
    int32_t s = micRawBuffer[i] >> 14;
    s = (int32_t)(s * MIC_GAIN_MULTIPLIER);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    converted[i] = (int16_t)s;
  }

  MicrophoneChunk c = { converted, sampleCount };
  if (xQueueSend(micSendQueue, &c, 0) != pdTRUE) {
    free(converted);
  }
}
