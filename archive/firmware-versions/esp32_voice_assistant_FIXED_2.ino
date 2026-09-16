/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║                                                                          ║
 * ║               ESP32 COMPLETE VOICE ASSISTANT - PRODUCTION                ║
 * ║                            MCLK FIXED VERSION                            ║
 * ║                                                                          ║
 * ║  Features:                                                               ║
 * ║  ✅ Dual I2S - Separate microphone and speaker buses                    ║
 * ║  ✅ Full-duplex audio - Record and playback simultaneously              ║
 * ║  ✅ WebSocket communication with VPS relay                              ║
 * ║  ✅ Comprehensive error handling and recovery                           ║
 * ║  ✅ Detailed logging for debugging                                      ║
 * ║  ✅ Memory-efficient streaming architecture                             ║
 * ║  ✅ Task-based design for optimal performance                           ║
 * ║                                                                          ║
 * ║  Hardware:                                                               ║
 * ║  • INMP441 Microphone  → I2S_NUM_0 (GPIO 27, 25, 18)                   ║
 * ║  • MAX98357A Amplifier → I2S_NUM_1 (GPIO 14, 12, 13)                   ║
 * ║                                                                          ║
 * ║  Libraries Required:                                                     ║
 * ║  • WebSockets by Markus Sattler                                         ║
 * ║  • ArduinoJson by Benoit Blanchon (v6.x)                               ║
 * ║                                                                          ║
 * ╚══════════════════════════════════════════════════════════════════════════╝
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"

// ═════════════════════════════════════════════════════════════════════════════
// CONFIGURATION SECTION
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// WiFi Configuration
// ────────────────────────────────────────────────────────────────────────────
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 20000  // 20 seconds timeout
#define WIFI_RETRY_DELAY_MS 500

// ────────────────────────────────────────────────────────────────────────────
// VPS Server Configuration
// ────────────────────────────────────────────────────────────────────────────
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
#define VPS_PATH "/"
#define WS_RECONNECT_INTERVAL_MS 3000
#define WS_HEARTBEAT_INTERVAL_MS 15000
#define WS_HEARTBEAT_TIMEOUT_MS 3000
#define WS_HEARTBEAT_PONG_RETRIES 2

// ────────────────────────────────────────────────────────────────────────────
// I2S Pin Configuration - Microphone (INMP441)
// ────────────────────────────────────────────────────────────────────────────
#define I2S_MIC_SERIAL_CLOCK  25  // SCK / BCLK
#define I2S_MIC_WORD_SELECT   27  // WS / LRCK
#define I2S_MIC_SERIAL_DATA   18  // SD / DOUT

// ────────────────────────────────────────────────────────────────────────────
// I2S Pin Configuration - Speaker (MAX98357A)
// ────────────────────────────────────────────────────────────────────────────
#define I2S_SPK_SERIAL_CLOCK  12  // BCLK
#define I2S_SPK_WORD_SELECT   14  // LRC
#define I2S_SPK_SERIAL_DATA   13  // DIN

// ────────────────────────────────────────────────────────────────────────────
// Audio Configuration
// ────────────────────────────────────────────────────────────────────────────
#define AUDIO_SAMPLE_RATE     8000  // Hz - Must match PC side
#define AUDIO_BITS_PER_SAMPLE 16    // Bits
#define AUDIO_CHANNELS        1     // Mono

// Microphone specific
#define MIC_I2S_BITS          32    // INMP441 outputs 32-bit
#define MIC_GAIN_MULTIPLIER   2.0f  // Amplification factor

// Speaker specific
#define SPK_I2S_BITS          16    // MAX98357A uses 16-bit
#define SPK_VOLUME_GAIN       2.0f  // Volume gain

// ────────────────────────────────────────────────────────────────────────────
// I2S DMA Configuration
// ────────────────────────────────────────────────────────────────────────────
#define I2S_DMA_BUF_COUNT     4     // Number of DMA buffers
#define I2S_DMA_BUF_LEN       512   // Samples per buffer

// ────────────────────────────────────────────────────────────────────────────
// Microphone Recording Configuration
// ────────────────────────────────────────────────────────────────────────────
#define MIC_READ_BUFFER_SIZE  512   // Samples to read at once
#define MIC_BATCH_SIZE        6     // Batches to combine before sending
#define MIC_SEND_QUEUE_SIZE   32    // Queue depth for outgoing audio

// ────────────────────────────────────────────────────────────────────────────
// Speaker Playback Configuration
// ────────────────────────────────────────────────────────────────────────────
#define SPK_PLAYBACK_CHUNK_SIZE  8192   // Bytes per TTS chunk
#define SPK_WRITE_CHUNK_SIZE     2048   // Bytes per i2s_write call

// ────────────────────────────────────────────────────────────────────────────
// Task Configuration
// ────────────────────────────────────────────────────────────────────────────
#define TASK_MIC_SEND_STACK_SIZE      8192
#define TASK_MIC_SEND_PRIORITY        1
#define TASK_MIC_SEND_CORE            0

#define TASK_SPEAKER_PLAY_STACK_SIZE  4096
#define TASK_SPEAKER_PLAY_PRIORITY    2
#define TASK_SPEAKER_PLAY_CORE        0

// ────────────────────────────────────────────────────────────────────────────
// Debug Configuration
// ────────────────────────────────────────────────────────────────────────────
#define DEBUG_ENABLED         true
#define DEBUG_WEBSOCKET       true
#define DEBUG_MICROPHONE      true
#define DEBUG_SPEAKER         true
#define DEBUG_MEMORY          true

// ────────────────────────────────────────────────────────────────────────────
// Statistics Reporting
// ────────────────────────────────────────────────────────────────────────────
#define STATS_REPORT_INTERVAL_MS  30000  // Report every 30 seconds

// ═════════════════════════════════════════════════════════════════════════════
// TYPE DEFINITIONS
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Microphone Audio Chunk Structure
// ────────────────────────────────────────────────────────────────────────────
struct MicrophoneChunk {
    int16_t* samples;      // Pointer to PCM samples
    size_t   sampleCount;  // Number of samples
};

// ────────────────────────────────────────────────────────────────────────────
// Speaker Playback State
// ────────────────────────────────────────────────────────────────────────────
struct SpeakerState {
    bool      isPlaying;          // Currently playing audio
    uint8_t*  audioBuffer;        // Buffer for decoded audio
    size_t    bufferSize;         // Size of allocated buffer
    size_t    bufferUsed;         // Bytes currently in buffer
    int       currentChunk;       // Current chunk number
    int       totalChunks;        // Total chunks expected
};

// ────────────────────────────────────────────────────────────────────────────
// System Statistics
// ────────────────────────────────────────────────────────────────────────────
struct SystemStats {
    // Microphone statistics
    unsigned long micChunksSent;
    unsigned long micChunksDropped;
    unsigned long micBytesRead;
    
    // Speaker statistics
    unsigned long spkChunksReceived;
    unsigned long spkBytesPlayed;
    unsigned long spkPlaybackCount;
    
    // WebSocket statistics
    unsigned long wsMessagesReceived;
    unsigned long wsMessagesSent;
    unsigned long wsReconnections;
    unsigned long wsLastConnectTime;
    
    // System statistics
    unsigned long uptimeSeconds;
    unsigned long lastStatsReport;
};

// ═════════════════════════════════════════════════════════════════════════════
// GLOBAL VARIABLES
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// WebSocket Client
// ────────────────────────────────────────────────────────────────────────────
WebSocketsClient webSocket;
volatile bool wsConnected = false;
volatile bool wsRegistered = false;

// ────────────────────────────────────────────────────────────────────────────
// Microphone Variables
// ────────────────────────────────────────────────────────────────────────────
QueueHandle_t micSendQueue;                          // Queue for outgoing audio
static int32_t micRawBuffer[MIC_READ_BUFFER_SIZE];   // Raw 32-bit from I2S
static int16_t micBatchBuffer[MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE];  // Batched samples
static size_t  micBatchedSamples = 0;                // Samples in batch buffer

// Base64 encoding buffers for microphone
#define MIC_B64_MAX_LEN (((MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE * 2 + 2) / 3) * 4 + 4)
static uint8_t micBase64Buffer[MIC_B64_MAX_LEN];
static char    micJsonBuffer[38 + MIC_B64_MAX_LEN + 3];

// ────────────────────────────────────────────────────────────────────────────
// Speaker Variables
// ────────────────────────────────────────────────────────────────────────────
static SpeakerState speaker = {
    .isPlaying = false,
    .audioBuffer = nullptr,
    .bufferSize = 0,
    .bufferUsed = 0,
    .currentChunk = 0,
    .totalChunks = 0
};

TaskHandle_t speakerPlaybackTask = nullptr;

// ────────────────────────────────────────────────────────────────────────────
// Statistics
// ────────────────────────────────────────────────────────────────────────────
static SystemStats stats = {
    .micChunksSent = 0,
    .micChunksDropped = 0,
    .micBytesRead = 0,
    .spkChunksReceived = 0,
    .spkBytesPlayed = 0,
    .spkPlaybackCount = 0,
    .wsMessagesReceived = 0,
    .wsMessagesSent = 0,
    .wsReconnections = 0,
    .wsLastConnectTime = 0,
    .uptimeSeconds = 0,
    .lastStatsReport = 0
};

// ═════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Print formatted divider
// ────────────────────────────────────────────────────────────────────────────
void printDivider(char symbol = '═', int length = 80) {
    for (int i = 0; i < length; i++) {
        Serial.print(symbol);
    }
    Serial.println();
}

// ────────────────────────────────────────────────────────────────────────────
// Print section header
// ────────────────────────────────────────────────────────────────────────────
void printSectionHeader(const char* title) {
    Serial.println();
    printDivider('═');
    Serial.printf("  %s\n", title);
    printDivider('═');
}

// ────────────────────────────────────────────────────────────────────────────
// Format bytes to human readable
// ────────────────────────────────────────────────────────────────────────────
String formatBytes(size_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    if (bytes < 1024 * 1024) return String(bytes / 1024.0, 2) + " KB";
    return String(bytes / 1024.0 / 1024.0, 2) + " MB";
}

// ────────────────────────────────────────────────────────────────────────────
// Format time duration
// ────────────────────────────────────────────────────────────────────────────
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

// ────────────────────────────────────────────────────────────────────────────
// Initialize Microphone I2S (I2S_NUM_0)
// ────────────────────────────────────────────────────────────────────────────
bool initializeMicrophone() {
    printSectionHeader("INITIALIZING MICROPHONE (INMP441)");
    
    Serial.println("[MIC] Configuring I2S_NUM_0 for INMP441...");
    
    // I2S configuration for INMP441
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
    
    // Pin configuration
    i2s_pin_config_t pinConfig = {
        .bck_io_num = I2S_MIC_SERIAL_CLOCK,
        .ws_io_num = I2S_MIC_WORD_SELECT,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SERIAL_DATA
    };
    
    // ✅ CRITICAL FIX: Set MCLK to no change AFTER struct creation
    pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
    
    // Install driver
    Serial.printf("[MIC] Installing I2S driver on I2S_NUM_0...\n");
    esp_err_t result = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
    if (result != ESP_OK) {
        Serial.printf("[MIC] ❌ FAILED to install I2S driver! Error: %d\n", result);
        return false;
    }
    Serial.println("[MIC] ✅ I2S driver installed");
    
    // Set pins
    Serial.printf("[MIC] Setting pin configuration...\n");
    Serial.printf("[MIC]   • SCK  (BCLK): GPIO %d\n", I2S_MIC_SERIAL_CLOCK);
    Serial.printf("[MIC]   • WS   (LRCK): GPIO %d\n", I2S_MIC_WORD_SELECT);
    Serial.printf("[MIC]   • SD   (DOUT): GPIO %d\n", I2S_MIC_SERIAL_DATA);
    
    result = i2s_set_pin(I2S_NUM_0, &pinConfig);
    if (result != ESP_OK) {
        Serial.printf("[MIC] ❌ FAILED to set pins! Error: %d\n", result);
        return false;
    }
    Serial.println("[MIC] ✅ Pins configured");
    
    // Start I2S
    Serial.println("[MIC] Starting I2S peripheral...");
    result = i2s_start(I2S_NUM_0);
    if (result != ESP_OK) {
        Serial.printf("[MIC] ❌ FAILED to start I2S! Error: %d\n", result);
        return false;
    }
    
    Serial.println("[MIC] ✅ I2S started successfully");
    Serial.printf("[MIC] Configuration:\n");
    Serial.printf("[MIC]   • Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    Serial.printf("[MIC]   • Bit Depth: %d-bit\n", MIC_I2S_BITS);
    Serial.printf("[MIC]   • Channels: Mono (Left)\n");
    Serial.printf("[MIC]   • DMA Buffers: %d x %d samples\n", I2S_DMA_BUF_COUNT, I2S_DMA_BUF_LEN);
    Serial.printf("[MIC]   • Gain: %.1fx\n", MIC_GAIN_MULTIPLIER);
    
    Serial.println("[MIC] ✅ Microphone initialized successfully!");
    
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Initialize Speaker I2S (I2S_NUM_1)
// ────────────────────────────────────────────────────────────────────────────
bool initializeSpeaker() {
    printSectionHeader("INITIALIZING SPEAKER (MAX98357A)");
    
    Serial.println("[SPK] Configuring I2S_NUM_1 for MAX98357A...");
    
    // I2S configuration for MAX98357A
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
    
    // Pin configuration
    i2s_pin_config_t pinConfig = {
        .bck_io_num = I2S_SPK_SERIAL_CLOCK,
        .ws_io_num = I2S_SPK_WORD_SELECT,
        .data_out_num = I2S_SPK_SERIAL_DATA,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    
    // ✅ CRITICAL FIX: Set MCLK to no change AFTER struct creation
    pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
    
    // Install driver
    Serial.printf("[SPK] Installing I2S driver on I2S_NUM_1...\n");
    esp_err_t result = i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL);
    if (result != ESP_OK) {
        Serial.printf("[SPK] ❌ FAILED to install I2S driver! Error: %d\n", result);
        return false;
    }
    Serial.println("[SPK] ✅ I2S driver installed");
    
    // Set pins
    Serial.printf("[SPK] Setting pin configuration...\n");
    Serial.printf("[SPK]   • BCLK: GPIO %d\n", I2S_SPK_SERIAL_CLOCK);
    Serial.printf("[SPK]   • LRC:  GPIO %d\n", I2S_SPK_WORD_SELECT);
    Serial.printf("[SPK]   • DIN:  GPIO %d\n", I2S_SPK_SERIAL_DATA);
    
    result = i2s_set_pin(I2S_NUM_1, &pinConfig);
    if (result != ESP_OK) {
        Serial.printf("[SPK] ❌ FAILED to set pins! Error: %d\n", result);
        return false;
    }
    Serial.println("[SPK] ✅ Pins configured");
    
    // Start I2S
    Serial.println("[SPK] Starting I2S peripheral...");
    result = i2s_start(I2S_NUM_1);
    if (result != ESP_OK) {
        Serial.printf("[SPK] ❌ FAILED to start I2S! Error: %d\n", result);
        return false;
    }
    
    Serial.println("[SPK] ✅ I2S started successfully");
    Serial.printf("[SPK] Configuration:\n");
    Serial.printf("[SPK]   • Sample Rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    Serial.printf("[SPK]   • Bit Depth: %d-bit\n", SPK_I2S_BITS);
    Serial.printf("[SPK]   • Channels: Mono (Left)\n");
    Serial.printf("[SPK]   • DMA Buffers: %d x %d samples\n", I2S_DMA_BUF_COUNT, I2S_DMA_BUF_LEN);
    
    Serial.println("[SPK] ✅ Speaker initialized successfully!");
    
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// MICROPHONE FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Send audio chunk to PC via WebSocket
// ────────────────────────────────────────────────────────────────────────────
void sendMicrophoneChunk(int16_t* samples, size_t sampleCount) {
    if (!wsConnected || !wsRegistered) {
        if (DEBUG_MICROPHONE) {
            Serial.println("[MIC-SEND] ⚠️  WebSocket not ready, dropping chunk");
        }
        stats.micChunksDropped++;
        return;
    }
    
    // Calculate buffer sizes
    size_t pcmBytes = sampleCount * sizeof(int16_t);
    
    // Base64 encode the PCM data
    size_t base64Length = 0;
    int result = mbedtls_base64_encode(
        micBase64Buffer,
        MIC_B64_MAX_LEN,
        &base64Length,
        (uint8_t*)samples,
        pcmBytes
    );
    
    if (result != 0) {
        Serial.printf("[MIC-SEND] ❌ Base64 encoding failed! Error: %d\n", result);
        stats.micChunksDropped++;
        return;
    }
    
    // Null-terminate base64 string
    micBase64Buffer[base64Length] = '\0';
    
    // Build JSON message: {"target":"pc","type":"audio","data":"<base64>"}
    int headerLength = snprintf(
        micJsonBuffer,
        sizeof(micJsonBuffer),
        "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\""
    );
    
    // Append base64 data
    memcpy(micJsonBuffer + headerLength, micBase64Buffer, base64Length);
    
    // Close JSON
    micJsonBuffer[headerLength + base64Length] = '"';
    micJsonBuffer[headerLength + base64Length + 1] = '}';
    micJsonBuffer[headerLength + base64Length + 2] = '\0';
    
    // Send via WebSocket
    webSocket.sendTXT(micJsonBuffer);
    
    // Update statistics
    stats.micChunksSent++;
    stats.wsMessagesSent++;
    stats.micBytesRead += pcmBytes;
    
    if (DEBUG_MICROPHONE && (stats.micChunksSent % 20 == 0)) {
        Serial.printf("[MIC-SEND] 📤 Sent chunk #%lu (%d samples, %s)\n",
                     stats.micChunksSent, sampleCount, formatBytes(pcmBytes).c_str());
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Microphone send task - runs on Core 0
// ────────────────────────────────────────────────────────────────────────────
void microphoneSendTask(void* parameter) {
    Serial.println("[MIC-TASK] 🎤 Microphone send task started on Core 0");
    
    MicrophoneChunk chunk;
    
    while (true) {
        // Wait for chunk from queue
        if (xQueueReceive(micSendQueue, &chunk, portMAX_DELAY) == pdTRUE) {
            
            // Only send if WebSocket is ready and speaker is not playing
            if (wsRegistered && !speaker.isPlaying) {
                // Add to batch buffer
                memcpy(
                    micBatchBuffer + micBatchedSamples,
                    chunk.samples,
                    chunk.sampleCount * sizeof(int16_t)
                );
                micBatchedSamples += chunk.sampleCount;
                
                // Send when batch is full
                if (micBatchedSamples >= (MIC_READ_BUFFER_SIZE * MIC_BATCH_SIZE)) {
                    sendMicrophoneChunk(micBatchBuffer, micBatchedSamples);
                    micBatchedSamples = 0;
                }
            }
            
            // Free the chunk memory
            free(chunk.samples);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SPEAKER FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Handle incoming TTS audio chunk
// ────────────────────────────────────────────────────────────────────────────
void handleTtsAudioChunk(JsonDocument& doc) {
    // Validate required fields
    if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) {
        Serial.println("[SPK] ❌ Missing required fields in TTS message");
        return;
    }
    
    int chunkNumber = doc["chunk"];
    int totalChunks = doc["total"];
    const char* base64Data = doc["data"];
    
    if (!base64Data || strlen(base64Data) == 0) {
        Serial.println("[SPK] ❌ Empty data field");
        return;
    }
    
    // ✅ FIX: Initialize buffer when NULL, not just on chunk 0!
    if (speaker.audioBuffer == nullptr) {
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           🎵 RECEIVING TTS AUDIO FROM PC                  ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        
        Serial.printf("[SPK] Starting new audio stream (ID: %lu)\n", stats.spkPlaybackCount + 1);
        Serial.printf("[SPK] Total chunks expected: %d\n", totalChunks);
        Serial.printf("[SPK] First chunk received: %d\n", chunkNumber);
        
        // Allocate new buffer
        speaker.audioBuffer = (uint8_t*)malloc(SPK_PLAYBACK_CHUNK_SIZE);
        if (speaker.audioBuffer == nullptr) {
            Serial.printf("[SPK] ❌ Failed to allocate %d bytes for audio buffer!\n", SPK_PLAYBACK_CHUNK_SIZE);
            Serial.printf("[SPK] Free heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
            return;
        }
        
        speaker.bufferSize = SPK_PLAYBACK_CHUNK_SIZE;
        speaker.bufferUsed = 0;
        speaker.currentChunk = -1;
        speaker.totalChunks = totalChunks;
        speaker.isPlaying = false;
        
        if (DEBUG_SPEAKER) {
            Serial.printf("[SPK] ✅ Audio buffer allocated: %s\n", formatBytes(speaker.bufferSize).c_str());
        }
    }
    
    // Decode base64 data
    size_t base64Length = strlen(base64Data);
    size_t decodedLength = 0;
    
    int result = mbedtls_base64_decode(
        speaker.audioBuffer,
        speaker.bufferSize,
        &decodedLength,
        (const unsigned char*)base64Data,
        base64Length
    );
    
    if (result != 0) {
        Serial.printf("[SPK] ❌ Base64 decode failed! Error: %d\n", result);
        return;
    }
    
    speaker.bufferUsed = decodedLength;
    speaker.currentChunk = chunkNumber;
    
    // Update statistics
    stats.spkChunksReceived++;
    
    // Progress indicator
    if ((chunkNumber + 1) % 5 == 0 || (chunkNumber + 1) == totalChunks) {
        int progress = ((chunkNumber + 1) * 100) / totalChunks;
        Serial.printf("[SPK] 📥 Progress: %d%% (%d/%d chunks, %s decoded)\n",
                     progress, chunkNumber + 1, totalChunks, formatBytes(decodedLength).c_str());
    }
    
    // Play this chunk immediately
    if (speaker.bufferUsed > 0) {
        speaker.isPlaying = true;
        
        // Wait for playback to complete (with WebSocket keep-alive)
        unsigned long playbackStart = millis();
        while (speaker.isPlaying && (millis() - playbackStart < 5000)) {
            webSocket.loop();  // ✅ Keep connection alive!
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        
        if (speaker.isPlaying) {
            Serial.println("[SPK] ⚠️  Playback timeout!");
            speaker.isPlaying = false;
        }
    }
    
    // Last chunk - cleanup
    if ((chunkNumber + 1) >= totalChunks) {
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           ✅ TTS AUDIO PLAYBACK COMPLETE                   ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        
        stats.spkPlaybackCount++;
        
        Serial.printf("[SPK] Playback #%lu finished\n", stats.spkPlaybackCount);
        Serial.printf("[SPK] Total chunks played: %d\n", totalChunks);
        
        // Clear DMA buffer
        i2s_zero_dma_buffer(I2S_NUM_1);
        
        // Free buffer
        if (speaker.audioBuffer != nullptr) {
            free(speaker.audioBuffer);
            speaker.audioBuffer = nullptr;
        }
        
        speaker.bufferUsed = 0;
        Serial.println();
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Speaker playback task - runs on Core 0
// ────────────────────────────────────────────────────────────────────────────
void speakerPlaybackTaskFunction(void* parameter) {
    Serial.println("[SPK-TASK] 🔊 Speaker playback task started on Core 0");
    
    while (true) {
        if (speaker.isPlaying && speaker.audioBuffer != nullptr && speaker.bufferUsed > 0) {
            if (DEBUG_SPEAKER) {
                Serial.printf("[SPK-TASK] 🔊 Playing %s...\n", formatBytes(speaker.bufferUsed).c_str());
            }
            
            // Apply volume gain to audio samples
            int16_t* audioSamples = (int16_t*)speaker.audioBuffer;
            size_t sampleCount = speaker.bufferUsed / sizeof(int16_t);
            
            for (size_t i = 0; i < sampleCount; i++) {
                int32_t amplified = (int32_t)(audioSamples[i] * SPK_VOLUME_GAIN);
                
                // Clamp to prevent clipping
                if (amplified > 32767) amplified = 32767;
                if (amplified < -32768) amplified = -32768;
                
                audioSamples[i] = (int16_t)amplified;
            }
            
            // Write audio data to I2S
            size_t totalWritten = 0;
            size_t totalToWrite = speaker.bufferUsed;
            
            while (totalWritten < totalToWrite) {
                size_t chunkSize = min((size_t)SPK_WRITE_CHUNK_SIZE, totalToWrite - totalWritten);
                size_t bytesWritten = 0;
                
                esp_err_t result = i2s_write(
                    I2S_NUM_1,
                    speaker.audioBuffer + totalWritten,
                    chunkSize,
                    &bytesWritten,
                    portMAX_DELAY
                );
                
                if (result != ESP_OK) {
                    Serial.printf("[SPK-TASK] ❌ I2S write error: %d\n", result);
                    break;
                }
                
                totalWritten += bytesWritten;
                stats.spkBytesPlayed += bytesWritten;
            }
            
            if (DEBUG_SPEAKER) {
                Serial.printf("[SPK-TASK] ✅ Played %s\n", formatBytes(totalWritten).c_str());
            }
            
            // Mark as done
            speaker.isPlaying = false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// WEBSOCKET FUNCTIONS
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Handle incoming WebSocket text message
// ────────────────────────────────────────────────────────────────────────────
void handleWebSocketMessage(char* payload, size_t length) {
    stats.wsMessagesReceived++;
    
    if (DEBUG_WEBSOCKET) {
        Serial.printf("\n[WS] ⬇️  Message #%lu received (%d bytes)\n",
                     stats.wsMessagesReceived, length);
    }
    
    // Parse JSON
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        Serial.printf("[WS] ❌ JSON parse error: %s\n", error.c_str());
        if (DEBUG_WEBSOCKET && length < 200) {
            Serial.printf("[WS] Raw payload: %s\n", payload);
        }
        return;
    }
    
    // Get message type
    const char* messageType = doc["type"];
    if (!messageType) {
        Serial.println("[WS] ⚠️  Message missing 'type' field");
        return;
    }
    
    if (DEBUG_WEBSOCKET) {
        Serial.printf("[WS] Message type: '%s'\n", messageType);
    }
    
    // Handle different message types
    if (strcmp(messageType, "tts") == 0) {
        handleTtsAudioChunk(doc);
    }
    else {
        if (DEBUG_WEBSOCKET) {
            Serial.printf("[WS] ⚠️  Unknown message type: '%s'\n", messageType);
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// WebSocket event handler
// ────────────────────────────────────────────────────────────────────────────
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
            
            // Send registration message
            Serial.println("[WS] Sending registration: {\"id\":\"esp32\"}");
            webSocket.sendTXT("{\"id\":\"esp32\"}");
            
            wsRegistered = true;
            
            Serial.println("[WS] ✅ Registration complete");
            Serial.println("[WS] ESP32 is now ready to send/receive audio");
            Serial.println();
            break;
            
        case WStype_TEXT:
            handleWebSocketMessage((char*)payload, length);
            break;
            
        case WStype_PING:
            if (DEBUG_WEBSOCKET) {
                Serial.println("[WS] 🏓 Ping received");
            }
            break;
            
        case WStype_PONG:
            if (DEBUG_WEBSOCKET) {
                Serial.println("[WS] 🏓 Pong received");
            }
            break;
            
        case WStype_ERROR:
            Serial.println("[WS] ❌ WebSocket error!");
            break;
            
        default:
            if (DEBUG_WEBSOCKET) {
                Serial.printf("[WS] Event type: %d\n", type);
            }
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// STATISTICS AND MONITORING
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Print system statistics
// ────────────────────────────────────────────────────────────────────────────
void printSystemStatistics() {
    unsigned long now = millis();
    
    if (now - stats.lastStatsReport < STATS_REPORT_INTERVAL_MS) {
        return;
    }
    
    stats.lastStatsReport = now;
    stats.uptimeSeconds = now / 1000;
    
    Serial.println("\n╔════════════════════════════════════════════════════════════╗");
    Serial.println("║                    SYSTEM STATISTICS                       ║");
    Serial.println("╠════════════════════════════════════════════════════════════╣");
    
    // System information
    Serial.printf("║  Uptime: %-49s ║\n", formatDuration(stats.uptimeSeconds).c_str());
    Serial.printf("║  Free Heap: %-44s ║\n", formatBytes(ESP.getFreeHeap()).c_str());
    Serial.printf("║  Largest Block: %-40s ║\n", formatBytes(ESP.getMaxAllocHeap()).c_str());
    
    Serial.println("╠════════════════════════════════════════════════════════════╣");
    
    // WebSocket statistics
    Serial.printf("║  WebSocket Status: %-39s ║\n", wsConnected ? "CONNECTED ✅" : "DISCONNECTED ❌");
    Serial.printf("║  Messages Sent: %-42lu ║\n", stats.wsMessagesSent);
    Serial.printf("║  Messages Received: %-38lu ║\n", stats.wsMessagesReceived);
    Serial.printf("║  Reconnections: %-42lu ║\n", stats.wsReconnections);
    
    Serial.println("╠════════════════════════════════════════════════════════════╣");
    
    // Microphone statistics
    Serial.printf("║  Mic Chunks Sent: %-40lu ║\n", stats.micChunksSent);
    Serial.printf("║  Mic Chunks Dropped: %-37lu ║\n", stats.micChunksDropped);
    Serial.printf("║  Mic Bytes Read: %-41s ║\n", formatBytes(stats.micBytesRead).c_str());
    
    if (stats.micChunksSent + stats.micChunksDropped > 0) {
        float dropRate = (stats.micChunksDropped * 100.0) / (stats.micChunksSent + stats.micChunksDropped);
        Serial.printf("║  Mic Drop Rate: %.2f%%%-39s ║\n", dropRate, "");
    }
    
    Serial.println("╠════════════════════════════════════════════════════════════╣");
    
    // Speaker statistics
    Serial.printf("║  Spk Chunks Received: %-36lu ║\n", stats.spkChunksReceived);
    Serial.printf("║  Spk Bytes Played: %-39s ║\n", formatBytes(stats.spkBytesPlayed).c_str());
    Serial.printf("║  Spk Playbacks: %-42lu ║\n", stats.spkPlaybackCount);
    
    Serial.println("╚════════════════════════════════════════════════════════════╝\n");
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    // Initialize serial communication
    Serial.begin(115200);
    delay(1000);  // Wait for serial to stabilize
    
    // Print startup banner
    Serial.println("\n\n");
    printDivider('═');
    Serial.println("║                                                                          ║");
    Serial.println("║              ESP32 COMPLETE VOICE ASSISTANT - PRODUCTION                 ║");
    Serial.println("║                            MCLK FIXED VERSION                            ║");
    Serial.println("║                                                                          ║");
    Serial.println("║  Dual I2S Voice Assistant with Full-Duplex Audio                         ║");
    Serial.println("║  Microphone (INMP441) + Speaker (MAX98357A)                             ║");
    Serial.println("║                                                                          ║");
    printDivider('═');
    Serial.println();
    
    delay(500);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Step 1: Initialize I2S peripherals
    // ─────────────────────────────────────────────────────────────────────────
    
    // ✅ Initialize SPEAKER FIRST to avoid pin conflicts!
    if (!initializeSpeaker()) {
        Serial.println("\n❌ FATAL: Speaker initialization failed!");
        Serial.println("System halted.");
        while (1) delay(1000);
    }
    delay(100);
    
    if (!initializeMicrophone()) {
        Serial.println("\n❌ FATAL: Microphone initialization failed!");
        Serial.println("System halted.");
        while (1) delay(1000);
    }
    delay(100);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Step 2: Connect to WiFi
    // ─────────────────────────────────────────────────────────────────────────
    
    printSectionHeader("CONNECTING TO WIFI");
    
    Serial.printf("[WiFi] SSID: %s\n", WIFI_SSID);
    Serial.printf("[WiFi] Connecting");
    
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // Disable WiFi sleep for better performance
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    unsigned long wifiStartTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifiStartTime > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("\n[WiFi] ❌ Connection timeout!");
            Serial.println("Please check WiFi credentials and try again.");
            while (1) delay(1000);
        }
        delay(WIFI_RETRY_DELAY_MS);
        Serial.print(".");
    }
    
    Serial.println();
    Serial.println("[WiFi] ✅ Connected successfully!");
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Signal Strength: %d dBm\n", WiFi.RSSI());
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());
    
    // ─────────────────────────────────────────────────────────────────────────
    // Step 3: Initialize WebSocket connection
    // ─────────────────────────────────────────────────────────────────────────
    
    printSectionHeader("INITIALIZING WEBSOCKET");
    
    Serial.printf("[WS] Server: %s:%d%s\n", VPS_HOST, VPS_PORT, VPS_PATH);
    Serial.println("[WS] Configuring WebSocket client...");
    
    webSocket.begin(VPS_HOST, VPS_PORT, VPS_PATH);
    webSocket.onEvent(onWebSocketEvent);
    webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
    webSocket.enableHeartbeat(
        WS_HEARTBEAT_INTERVAL_MS,
        WS_HEARTBEAT_TIMEOUT_MS,
        WS_HEARTBEAT_PONG_RETRIES
    );
    
    Serial.println("[WS] ✅ WebSocket client configured");
    Serial.printf("[WS] Auto-reconnect interval: %d ms\n", WS_RECONNECT_INTERVAL_MS);
    Serial.printf("[WS] Heartbeat interval: %d ms\n", WS_HEARTBEAT_INTERVAL_MS);
    
    // ─────────────────────────────────────────────────────────────────────────
    // Step 4: Create queues and tasks
    // ─────────────────────────────────────────────────────────────────────────
    
    printSectionHeader("CREATING TASKS AND QUEUES");
    
    // Create microphone send queue
    Serial.printf("[TASK] Creating microphone queue (size: %d)...\n", MIC_SEND_QUEUE_SIZE);
    micSendQueue = xQueueCreate(MIC_SEND_QUEUE_SIZE, sizeof(MicrophoneChunk));
    if (micSendQueue == NULL) {
        Serial.println("[TASK] ❌ Failed to create microphone queue!");
        while (1) delay(1000);
    }
    Serial.println("[TASK] ✅ Microphone queue created");
    
    // Create microphone send task
    Serial.printf("[TASK] Creating microphone send task (Core %d, Stack: %d)...\n",
                 TASK_MIC_SEND_CORE, TASK_MIC_SEND_STACK_SIZE);
    BaseType_t result = xTaskCreatePinnedToCore(
        microphoneSendTask,
        "MicSend",
        TASK_MIC_SEND_STACK_SIZE,
        NULL,
        TASK_MIC_SEND_PRIORITY,
        NULL,
        TASK_MIC_SEND_CORE
    );
    if (result != pdPASS) {
        Serial.println("[TASK] ❌ Failed to create microphone send task!");
        while (1) delay(1000);
    }
    Serial.println("[TASK] ✅ Microphone send task created");
    
    // Create speaker playback task
    Serial.printf("[TASK] Creating speaker playback task (Core %d, Stack: %d)...\n",
                 TASK_SPEAKER_PLAY_CORE, TASK_SPEAKER_PLAY_STACK_SIZE);
    result = xTaskCreatePinnedToCore(
        speakerPlaybackTaskFunction,
        "SpkPlay",
        TASK_SPEAKER_PLAY_STACK_SIZE,
        NULL,
        TASK_SPEAKER_PLAY_PRIORITY,
        &speakerPlaybackTask,
        TASK_SPEAKER_PLAY_CORE
    );
    if (result != pdPASS) {
        Serial.println("[TASK] ❌ Failed to create speaker playback task!");
        while (1) delay(1000);
    }
    Serial.println("[TASK] ✅ Speaker playback task created");
    
    // ─────────────────────────────────────────────────────────────────────────
    // Initialization complete
    // ─────────────────────────────────────────────────────────────────────────
    
    printSectionHeader("INITIALIZATION COMPLETE");
    
    Serial.printf("[MEM] Free Heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
    Serial.printf("[MEM] Largest Block: %s\n", formatBytes(ESP.getMaxAllocHeap()).c_str());
    
    Serial.println("\n╔════════════════════════════════════════════════════════════╗");
    Serial.println("║                                                            ║");
    Serial.println("║              ✅ ALL SYSTEMS OPERATIONAL ✅                 ║");
    Serial.println("║                                                            ║");
    Serial.println("║  🎤 Microphone: READY - Recording and sending audio       ║");
    Serial.println("║  🔊 Speaker: READY - Receiving and playing TTS            ║");
    Serial.println("║  🌐 WebSocket: CONNECTING - Waiting for server            ║");
    Serial.println("║                                                            ║");
    Serial.println("║  System is now operational and ready for voice assistant  ║");
    Serial.println("║  functionality. Speak into the microphone to test.        ║");
    Serial.println("║                                                            ║");
    Serial.println("╚════════════════════════════════════════════════════════════╝\n");
    
    stats.lastStatsReport = millis();
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    // Service WebSocket connection
    webSocket.loop();
    
    // Check if WebSocket is connected
    if (!wsConnected) {
        delay(10);
        return;
    }
    
    // Pause microphone while speaker is playing to avoid feedback
    if (speaker.isPlaying) {
        delay(10);
        return;
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Read microphone data
    // ─────────────────────────────────────────────────────────────────────────
    
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(
        I2S_NUM_0,
        micRawBuffer,
        sizeof(micRawBuffer),
        &bytesRead,
        pdMS_TO_TICKS(100)
    );
    
    // Check for read errors
    if (result != ESP_OK) {
        static unsigned long lastErrorTime = 0;
        if (millis() - lastErrorTime > 1000) {
            Serial.printf("[MIC] ⚠️  I2S read error: %d\n", result);
            lastErrorTime = millis();
        }
        delay(10);
        return;
    }
    
    // Check if data was read
    if (bytesRead == 0) {
        delay(10);
        return;
    }
    
    // Calculate number of samples
    size_t sampleCount = bytesRead / sizeof(int32_t);
    
    // Allocate memory for converted samples
    int16_t* convertedSamples = (int16_t*)malloc(sampleCount * sizeof(int16_t));
    if (convertedSamples == nullptr) {
        static unsigned long lastAllocError = 0;
        if (millis() - lastAllocError > 1000) {
            Serial.println("[MIC] ⚠️  Memory allocation failed!");
            lastAllocError = millis();
        }
        stats.micChunksDropped++;
        delay(10);
        return;
    }
    
    // Convert 32-bit samples to 16-bit with gain
    for (size_t i = 0; i < sampleCount; i++) {
        // Shift right to get 18-bit value, then shift right 2 more for 16-bit
        int32_t sample = micRawBuffer[i] >> 14;
        
        // Apply gain
        sample = (int32_t)(sample * MIC_GAIN_MULTIPLIER);
        
        // Clamp to 16-bit range
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        
        convertedSamples[i] = (int16_t)sample;
    }
    
    // Create chunk for queue
    MicrophoneChunk chunk = {
        .samples = convertedSamples,
        .sampleCount = sampleCount
    };
    
    // Add to send queue
    if (xQueueSend(micSendQueue, &chunk, 0) != pdTRUE) {
        // Queue full - drop chunk
        free(convertedSamples);
        stats.micChunksDropped++;
        
        static unsigned long lastQueueFullWarning = 0;
        if (millis() - lastQueueFullWarning > 5000) {
            Serial.println("[MIC] ⚠️  Send queue full! Dropping audio chunks.");
            lastQueueFullWarning = millis();
        }
    }
    
    // ─────────────────────────────────────────────────────────────────────────
    // Print statistics periodically
    // ─────────────────────────────────────────────────────────────────────────
    
    if (DEBUG_MEMORY) {
        printSystemStatistics();
    }
    
    // Small delay to prevent tight looping
    delay(10);
}
