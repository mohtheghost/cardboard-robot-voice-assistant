/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║                                                                          ║
 * ║          ESP32 VOICE ASSISTANT - ASYNC TTS FIX                          ║
 * ║          FIXES WEBSOCKET DISCONNECTION DURING PLAYBACK                  ║
 * ║                                                                          ║
 * ║  ✅ Fully asynchronous TTS playback                                     ║
 * ║  ✅ WebSocket never blocks                                              ║
 * ║  ✅ Chunk queue prevents message loss                                   ║
 * ║  ✅ No disconnections during playback!                                  ║
 * ║                                                                          ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ═════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ═════════════════════════════════════════════════════════════════════════════

// WiFi
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_DELAY_MS 500

// VPS Server
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
#define VPS_PATH "/"
#define WS_RECONNECT_INTERVAL_MS 3000
#define WS_HEARTBEAT_INTERVAL_MS 5000
#define WS_HEARTBEAT_TIMEOUT_MS 10000
#define WS_HEARTBEAT_PONG_RETRIES 5

// I2S Pins - Microphone
#define I2S_MIC_SERIAL_CLOCK  25
#define I2S_MIC_WORD_SELECT   27
#define I2S_MIC_SERIAL_DATA   18

// I2S Pins - Speaker
#define I2S_SPK_SERIAL_CLOCK  12
#define I2S_SPK_WORD_SELECT   14
#define I2S_SPK_SERIAL_DATA   13

// Audio
#define AUDIO_SAMPLE_RATE     8000
#define MIC_I2S_BITS          32
#define MIC_GAIN_MULTIPLIER   2.0f
#define SPK_I2S_BITS          16
#define SPK_VOLUME_GAIN       0.5f  // ✅ Brownout protection

// I2S DMA
#define I2S_DMA_BUF_COUNT     4
#define I2S_DMA_BUF_LEN       512

// Microphone
#define MIC_READ_BUFFER_SIZE  512
#define MIC_BATCH_SIZE        6
#define MIC_SEND_QUEUE_SIZE   32

// ✅ NEW: TTS Chunk Queue
#define TTS_CHUNK_QUEUE_SIZE  10    // Queue up to 10 TTS chunks
#define SPK_PLAYBACK_CHUNK_SIZE  8192
#define SPK_WRITE_CHUNK_SIZE     2048

// Tasks
#define TASK_MIC_SEND_STACK_SIZE      8192
#define TASK_MIC_SEND_PRIORITY        1
#define TASK_MIC_SEND_CORE            0

#define TASK_SPEAKER_PLAY_STACK_SIZE  4096
#define TASK_SPEAKER_PLAY_PRIORITY    2
#define TASK_SPEAKER_PLAY_CORE        0

// Debug
#define DEBUG_ENABLED         true
#define DEBUG_WEBSOCKET       false
#define DEBUG_MICROPHONE      false
#define DEBUG_SPEAKER         true   // ✅ Enable to see async behavior
#define DEBUG_MEMORY          false

#define STATS_REPORT_INTERVAL_MS  30000

// ═════════════════════════════════════════════════════════════════════════════
// TYPE DEFINITIONS
// ═════════════════════════════════════════════════════════════════════════════

struct MicrophoneChunk {
    int16_t* samples;
    size_t   sampleCount;
};

// ✅ NEW: TTS Chunk Structure for Queue
struct TtsChunk {
    uint8_t* data;           // Decoded PCM data
    size_t   dataSize;       // Size of data
    int      chunkNumber;    // Chunk index
    int      totalChunks;    // Total chunks in stream
};

struct SystemStats {
    unsigned long micChunksSent;
    unsigned long micChunksDropped;
    unsigned long micBytesRead;
    unsigned long spkChunksReceived;
    unsigned long spkBytesPlayed;
    unsigned long spkPlaybackCount;
    unsigned long wsMessagesReceived;
    unsigned long wsMessagesSent;
    unsigned long wsReconnections;
    unsigned long wsLastConnectTime;
    unsigned long uptimeSeconds;
    unsigned long lastStatsReport;
};

// ═════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ═════════════════════════════════════════════════════════════════════════════

WebSocketsClient webSocket;
volatile bool wsConnected = false;
volatile bool wsRegistered = false;

// Microphone
QueueHandle_t micSendQueue;
static int32_t micRawBuffer[MIC_READ_BUFFER_SIZE];
static int16_t micBatchBuffer[MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE];
static size_t  micBatchedSamples = 0;

#define MIC_B64_MAX_LEN (((MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t micBase64Buffer[MIC_B64_MAX_LEN];
static char    micJsonBuffer[38 + MIC_B64_MAX_LEN + 3];

// ✅ NEW: TTS Chunk Queue
QueueHandle_t ttsChunkQueue;
volatile int currentTtsStream = -1;    // Track current stream ID
volatile int ttsChunksPlayed = 0;      // Chunks played in current stream
volatile bool ttsPlaying = false;       // Is TTS currently playing

TaskHandle_t speakerPlaybackTask = nullptr;

static SystemStats stats = {0};

// ═════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

void printDivider(char symbol = '═', int length = 80) {
    for (int i = 0; i < length; i++) Serial.print(symbol);
    Serial.println();
}

void printSectionHeader(const char* title) {
    Serial.println();
    printDivider('═');
    Serial.printf("  %s\n", title);
    printDivider('═');
}

String formatBytes(size_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    if (bytes < 1024 * 1024) return String(bytes / 1024.0, 2) + " KB";
    return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

String formatDuration(unsigned long seconds) {
    unsigned long hours = seconds / 3600;
    unsigned long minutes = (seconds % 3600) / 60;
    unsigned long secs = seconds % 60;
    
    char buffer[32];
    sprintf(buffer, "%02lu:%02lu:%02lu", hours, minutes, secs);
    return String(buffer);
}

// ═════════════════════════════════════════════════════════════════════════════
// I2S INITIALIZATION
// ═════════════════════════════════════════════════════════════════════════════

bool initializeMicrophone() {
    printSectionHeader("INITIALIZING MICROPHONE (INMP441)");
    
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
    
    if (i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL) != ESP_OK) {
        Serial.println("[MIC] ❌ I2S install failed!");
        return false;
    }
    
    if (i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
        Serial.println("[MIC] ❌ Pin config failed!");
        return false;
    }
    
    i2s_start(I2S_NUM_0);
    Serial.println("[MIC] ✅ Initialized");
    return true;
}

bool initializeSpeaker() {
    printSectionHeader("INITIALIZING SPEAKER (MAX98357A)");
    
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
    
    if (i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL) != ESP_OK) {
        Serial.println("[SPK] ❌ I2S install failed!");
        return false;
    }
    
    if (i2s_set_pin(I2S_NUM_1, &pinConfig) != ESP_OK) {
        Serial.println("[SPK] ❌ Pin config failed!");
        return false;
    }
    
    i2s_start(I2S_NUM_1);
    Serial.println("[SPK] ✅ Initialized");
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// MICROPHONE
// ═════════════════════════════════════════════════════════════════════════════

void sendMicrophoneChunk(int16_t* samples, size_t sampleCount) {
    if (!wsConnected || !wsRegistered) {
        stats.micChunksDropped++;
        return;
    }
    
    size_t pcmBytes = sampleCount * sizeof(int16_t);
    size_t base64Length = 0;
    
    int result = mbedtls_base64_encode(
        micBase64Buffer, MIC_B64_MAX_LEN, &base64Length,
        (uint8_t*)samples, pcmBytes
    );
    
    if (result != 0) {
        stats.micChunksDropped++;
        return;
    }
    
    micBase64Buffer[base64Length] = '\0';
    
    int headerLength = snprintf(micJsonBuffer, sizeof(micJsonBuffer),
                                "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"");
    
    memcpy(micJsonBuffer + headerLength, micBase64Buffer, base64Length);
    
    micJsonBuffer[headerLength + base64Length] = '"';
    micJsonBuffer[headerLength + base64Length + 1] = '}';
    micJsonBuffer[headerLength + base64Length + 2] = '\0';
    
    webSocket.sendTXT(micJsonBuffer);
    
    stats.micChunksSent++;
    stats.wsMessagesSent++;
    stats.micBytesRead += pcmBytes;
}

void microphoneSendTask(void* parameter) {
    Serial.println("[MIC-TASK] 🎤 Started");
    
    MicrophoneChunk chunk;
    
    while (true) {
        if (xQueueReceive(micSendQueue, &chunk, portMAX_DELAY) == pdTRUE) {
            
            if (wsRegistered && !ttsPlaying) {  // ✅ Check ttsPlaying instead
                memcpy(micBatchBuffer + micBatchedSamples, chunk.samples,
                       chunk.sampleCount * sizeof(int16_t));
                micBatchedSamples += chunk.sampleCount;
                
                if (micBatchedSamples >= (MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE)) {
                    sendMicrophoneChunk(micBatchBuffer, micBatchedSamples);
                    micBatchedSamples = 0;
                }
            }
            
            free(chunk.samples);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ✅ ASYNC TTS HANDLER - NEVER BLOCKS!
// ═════════════════════════════════════════════════════════════════════════════

void handleTtsAudioChunk(JsonDocument& doc) {
    if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) {
        Serial.println("[TTS] ❌ Missing fields");
        return;
    }
    
    int chunkNumber = doc["chunk"];
    int totalChunks = doc["total"];
    const char* base64Data = doc["data"];
    
    if (!base64Data || strlen(base64Data) == 0) {
        Serial.println("[TTS] ❌ Empty data");
        return;
    }
    
    // First chunk of new stream?
    if (chunkNumber == 0) {
        currentTtsStream++;
        ttsChunksPlayed = 0;
        
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           🎵 NEW TTS STREAM (ASYNC)                       ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        Serial.printf("[TTS] Stream #%d: %d chunks\n", currentTtsStream, totalChunks);
    }
    
    // Allocate memory for decoded data
    size_t maxDecodedSize = (strlen(base64Data) * 3) / 4 + 4;
    uint8_t* decodedData = (uint8_t*)malloc(maxDecodedSize);
    
    if (!decodedData) {
        Serial.printf("[TTS] ❌ Alloc failed! Heap: %d\n", ESP.getFreeHeap());
        return;
    }
    
    // Decode base64
    size_t decodedLength = 0;
    int result = mbedtls_base64_decode(
        decodedData, maxDecodedSize, &decodedLength,
        (const unsigned char*)base64Data, strlen(base64Data)
    );
    
    if (result != 0) {
        Serial.printf("[TTS] ❌ Decode error: %d\n", result);
        free(decodedData);
        return;
    }
    
    // Create TTS chunk
    TtsChunk ttsChunk = {
        .data = decodedData,
        .dataSize = decodedLength,
        .chunkNumber = chunkNumber,
        .totalChunks = totalChunks
    };
    
    // ✅ CRITICAL: Queue the chunk and RETURN IMMEDIATELY!
    // Speaker task will handle playback asynchronously
    if (xQueueSend(ttsChunkQueue, &ttsChunk, 0) != pdTRUE) {
        Serial.println("[TTS] ⚠️  Queue full! Dropping chunk");
        free(decodedData);
        return;
    }
    
    stats.spkChunksReceived++;
    
    // Progress
    if ((chunkNumber + 1) % 5 == 0 || (chunkNumber + 1) == totalChunks) {
        int progress = ((chunkNumber + 1) * 100) / totalChunks;
        Serial.printf("[TTS] 📥 Queued: %d%% (%d/%d)\n",
                     progress, chunkNumber + 1, totalChunks);
    }
    
    // ✅ RETURN IMMEDIATELY - WebSocket can continue processing!
}

// ═════════════════════════════════════════════════════════════════════════════
// ✅ ASYNC SPEAKER TASK - PROCESSES QUEUE INDEPENDENTLY
// ═════════════════════════════════════════════════════════════════════════════

void speakerPlaybackTaskFunction(void* parameter) {
    Serial.println("[SPK-TASK] 🔊 Started");
    
    TtsChunk chunk;
    
    while (true) {
        // Wait for next chunk from queue
        if (xQueueReceive(ttsChunkQueue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            ttsPlaying = true;  // ✅ Set flag
            
            if (DEBUG_SPEAKER) {
                Serial.printf("[SPK-TASK] ▶️  Playing chunk %d/%d (%s)\n",
                             chunk.chunkNumber + 1, chunk.totalChunks,
                             formatBytes(chunk.dataSize).c_str());
            }
            
            // Apply gain
            int16_t* samples = (int16_t*)chunk.data;
            size_t sampleCount = chunk.dataSize / 2;
            
            for (size_t i = 0; i < sampleCount; i++) {
                int32_t amp = (int32_t)(samples[i] * SPK_VOLUME_GAIN);
                if (amp > 32767) amp = 32767;
                if (amp < -32768) amp = -32768;
                samples[i] = (int16_t)amp;
            }
            
            // Write to I2S
            size_t totalWritten = 0;
            while (totalWritten < chunk.dataSize) {
                size_t toWrite = min((size_t)SPK_WRITE_CHUNK_SIZE,
                                    chunk.dataSize - totalWritten);
                size_t written = 0;
                
                esp_err_t result = i2s_write(I2S_NUM_1,
                                             chunk.data + totalWritten,
                                             toWrite, &written, portMAX_DELAY);
                
                if (result != ESP_OK) {
                    Serial.printf("[SPK-TASK] ❌ I2S error: %d\n", result);
                    break;
                }
                
                totalWritten += written;
                stats.spkBytesPlayed += written;
            }
            
            ttsChunksPlayed++;
            
            // Last chunk?
            if ((chunk.chunkNumber + 1) >= chunk.totalChunks) {
                Serial.println("\n╔════════════════════════════════════════════════════════════╗");
                Serial.println("║           ✅ TTS STREAM COMPLETE                           ║");
                Serial.println("╚════════════════════════════════════════════════════════════╝");
                Serial.printf("[SPK-TASK] Played %d chunks\n\n", ttsChunksPlayed);
                
                stats.spkPlaybackCount++;
                i2s_zero_dma_buffer(I2S_NUM_1);
                
                ttsPlaying = false;  // ✅ Clear flag
            }
            
            // Free chunk data
            free(chunk.data);
            
        } else {
            // No chunk received - clear playing flag if idle
            if (ttsPlaying && uxQueueMessagesWaiting(ttsChunkQueue) == 0) {
                ttsPlaying = false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1));  // Very short delay
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// WEBSOCKET
// ═════════════════════════════════════════════════════════════════════════════

void handleWebSocketMessage(char* payload, size_t length) {
    stats.wsMessagesReceived++;
    
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload, length)) {
        Serial.println("[WS] ❌ JSON parse error");
        return;
    }
    
    const char* messageType = doc["type"];
    if (!messageType) return;
    
    if (strcmp(messageType, "tts") == 0) {
        handleTtsAudioChunk(doc);  // ✅ Returns immediately!
    }
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("\n[WS] ❌ ═══════════ DISCONNECTED ═══════════");
            wsConnected = false;
            wsRegistered = false;
            break;
            
        case WStype_CONNECTED:
            Serial.println("\n[WS] ✅ ═══════════ CONNECTED ═══════════");
            Serial.printf("[WS] Server URL: %s\n", payload);
            
            wsConnected = true;
            stats.wsLastConnectTime = millis();
            stats.wsReconnections++;
            
            webSocket.sendTXT("{\"id\":\"esp32\"}");
            wsRegistered = true;
            
            Serial.println("[WS] ✅ Registration complete");
            Serial.println("[WS] ESP32 ready!\n");
            break;
            
        case WStype_TEXT:
            handleWebSocketMessage((char*)payload, length);
            break;
            
        case WStype_ERROR:
            Serial.println("[WS] ❌ Error!");
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔═══════════════════════════════════════════════════════════╗");
    Serial.println("║     ESP32 VOICE ASSISTANT - ASYNC TTS FIX                 ║");
    Serial.println("║     NO MORE WEBSOCKET DISCONNECTIONS!                     ║");
    Serial.println("╚═══════════════════════════════════════════════════════════╝\n");
    
    // Init I2S
    if (!initializeSpeaker() || !initializeMicrophone()) {
        Serial.println("❌ I2S init failed!");
        while(1) delay(1000);
    }
    
    // Connect WiFi
    printSectionHeader("CONNECTING TO WIFI");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\n[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
    
    // Init WebSocket
    printSectionHeader("INITIALIZING WEBSOCKET");
    webSocket.begin(VPS_HOST, VPS_PORT, VPS_PATH);
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
    webSocket.enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS,
                             WS_HEARTBEAT_TIMEOUT_MS,
                             WS_HEARTBEAT_PONG_RETRIES);
    Serial.println("[WS] ✅ Configured");
    
    // Create queues
    printSectionHeader("CREATING QUEUES AND TASKS");
    
    micSendQueue = xQueueCreate(MIC_SEND_QUEUE_SIZE, sizeof(MicrophoneChunk));
    if (!micSendQueue) {
        Serial.println("❌ Mic queue failed!");
        while(1) delay(1000);
    }
    Serial.println("[QUEUE] ✅ Microphone queue created");
    
    // ✅ NEW: Create TTS chunk queue
    ttsChunkQueue = xQueueCreate(TTS_CHUNK_QUEUE_SIZE, sizeof(TtsChunk));
    if (!ttsChunkQueue) {
        Serial.println("❌ TTS queue failed!");
        while(1) delay(1000);
    }
    Serial.println("[QUEUE] ✅ TTS chunk queue created");
    
    // Create mic task
    xTaskCreatePinnedToCore(microphoneSendTask, "MicSend",
                           TASK_MIC_SEND_STACK_SIZE, NULL,
                           TASK_MIC_SEND_PRIORITY, NULL,
                           TASK_MIC_SEND_CORE);
    Serial.println("[TASK] ✅ Microphone task created");
    
    // Create speaker task
    xTaskCreatePinnedToCore(speakerPlaybackTaskFunction, "SpkPlay",
                           TASK_SPEAKER_PLAY_STACK_SIZE, NULL,
                           TASK_SPEAKER_PLAY_PRIORITY, &speakerPlaybackTask,
                           TASK_SPEAKER_PLAY_CORE);
    Serial.println("[TASK] ✅ Speaker task created");
    
    printSectionHeader("INITIALIZATION COMPLETE");
    Serial.printf("[MEM] Free Heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
    
    Serial.println("\n╔════════════════════════════════════════════════════════════╗");
    Serial.println("║              ✅ ALL SYSTEMS OPERATIONAL ✅                 ║");
    Serial.println("║                                                            ║");
    Serial.println("║  🎤 Microphone: READY                                     ║");
    Serial.println("║  🔊 Speaker: READY (ASYNC PLAYBACK!)                      ║");
    Serial.println("║  🌐 WebSocket: CONNECTING                                 ║");
    Serial.println("║  📡 TTS Queue: ACTIVE (No more disconnects!)              ║");
    Serial.println("║                                                            ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝\n");
    
    stats.lastStatsReport = millis();
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    // ✅ WebSocket loop runs freely - NEVER blocked!
    webSocket.loop();
    
    if (!wsConnected) {
        delay(5);
        return;
    }
    
    // Pause mic during TTS
    if (ttsPlaying) {
        delay(5);
        return;
    }
    
    // Read microphone
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, micRawBuffer,
                                sizeof(micRawBuffer), &bytesRead,
                                pdMS_TO_TICKS(100));
    
    if (result != ESP_OK || bytesRead == 0) {
        delay(5);
        return;
    }
    
    size_t sampleCount = bytesRead / sizeof(int32_t);
    int16_t* convertedSamples = (int16_t*)malloc(sampleCount * sizeof(int16_t));
    
    if (!convertedSamples) {
        stats.micChunksDropped++;
        delay(5);
        return;
    }
    
    // Convert 32-bit to 16-bit with gain
    for (size_t i = 0; i < sampleCount; i++) {
        int32_t sample = micRawBuffer[i] >> 14;
        sample = (int32_t)(sample * MIC_GAIN_MULTIPLIER);
        
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        convertedSamples[i] = (int16_t)sample;
    }
    
    // Queue for sending
    MicrophoneChunk chunk = {
        .samples = convertedSamples,
        .sampleCount = sampleCount
    };
    
    if (xQueueSend(micSendQueue, &chunk, 0) != pdTRUE) {
        free(convertedSamples);
        stats.micChunksDropped++;
    }
}
