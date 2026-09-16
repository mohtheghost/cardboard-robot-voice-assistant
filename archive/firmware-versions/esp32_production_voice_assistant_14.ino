/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║                                                                          ║
 * ║               ESP32 COMPLETE VOICE ASSISTANT - PRODUCTION                ║
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
#include "soc/soc.h"           // ✅ For brownout detector
#include "soc/rtc_cntl_reg.h"  // ✅ For brownout detector

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
// Power Management
// ────────────────────────────────────────────────────────────────────────────
#define DISABLE_BROWNOUT_DETECTOR false  // Set to true ONLY for testing (not recommended!)

// ────────────────────────────────────────────────────────────────────────────
// VPS Server Configuration
// ────────────────────────────────────────────────────────────────────────────
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
#define VPS_PATH "/"
#define WS_RECONNECT_INTERVAL_MS 3000
#define WS_HEARTBEAT_INTERVAL_MS 5000   // ✅ More frequent heartbeats (was 15000)
#define WS_HEARTBEAT_TIMEOUT_MS 10000   // ✅ Longer timeout (was 3000)
#define WS_HEARTBEAT_PONG_RETRIES 5     // ✅ More retries (was 2)

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
#define SPK_VOLUME_GAIN       2.0f  // ✅ REDUCED from 5.0x to save power!

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
#define MIC_SEND_QUEUE_SIZE   16    // ✅ Reduced from 32 to save RAM for TTS

// ────────────────────────────────────────────────────────────────────────────
// Speaker Playback Configuration
// ────────────────────────────────────────────────────────────────────────────
#define SPK_PLAYBACK_CHUNK_SIZE  8192   // Bytes per TTS chunk
#define SPK_WRITE_CHUNK_SIZE     2048   // Bytes per i2s_write call
#define SPK_CHUNK_QUEUE_SIZE     10     // ✅ NEW: Queue for incoming chunks

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
#define DEBUG_DIAGNOSTICS     true  // ✅ NEW: Full diagnostics mode

// ────────────────────────────────────────────────────────────────────────────
// Statistics Reporting
// ────────────────────────────────────────────────────────────────────────────
#define STATS_REPORT_INTERVAL_MS  30000  // Report every 30 seconds
#define DIAGNOSTICS_INTERVAL_MS   10000  // ✅ NEW: Detailed diagnostics every 10s

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
// Speaker Audio Chunk Structure
// ────────────────────────────────────────────────────────────────────────────
struct SpeakerChunk {
    uint8_t* data;         // Pointer to PCM data
    size_t   dataSize;     // Size of data
    int      chunkNumber;  // Chunk sequence number
    int      totalChunks;  // Total chunks in stream
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
    unsigned long micLastReadTime;
    unsigned long micReadErrors;
    
    // Speaker statistics
    unsigned long spkChunksReceived;
    unsigned long spkBytesPlayed;
    unsigned long spkPlaybackCount;
    unsigned long spkLastPlayTime;
    unsigned long spkDecodeErrors;
    
    // WebSocket statistics
    unsigned long wsMessagesReceived;
    unsigned long wsMessagesSent;
    unsigned long wsReconnections;
    unsigned long wsLastConnectTime;
    unsigned long wsLastMessageTime;
    unsigned long wsErrors;
    
    // System statistics
    unsigned long uptimeSeconds;
    unsigned long lastStatsReport;
    unsigned long lastDiagnostics;
    size_t minFreeHeap;
    size_t currentFreeHeap;
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
    .micLastReadTime = 0,
    .micReadErrors = 0,
    .spkChunksReceived = 0,
    .spkBytesPlayed = 0,
    .spkPlaybackCount = 0,
    .spkLastPlayTime = 0,
    .spkDecodeErrors = 0,
    .wsMessagesReceived = 0,
    .wsMessagesSent = 0,
    .wsReconnections = 0,
    .wsLastConnectTime = 0,
    .wsLastMessageTime = 0,
    .wsErrors = 0,
    .uptimeSeconds = 0,
    .lastStatsReport = 0,
    .lastDiagnostics = 0,
    .minFreeHeap = 0,
    .currentFreeHeap = 0
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
    
    // ✅ Set MCLK to no change AFTER struct creation to avoid field order issues
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
    
    // ✅ Set MCLK to no change AFTER struct creation to avoid field order issues
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
    
    // ✅ STREAMING MODE: Allocate small buffer for ONE chunk at a time
    const size_t CHUNK_BUFFER_SIZE = 8192;  // 8KB per chunk
    
    // First chunk of new stream
    if (chunkNumber == 0 || speaker.currentChunk == -1 || speaker.audioBuffer == nullptr) {
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           🎵 RECEIVING TTS AUDIO FROM PC                  ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        
        Serial.printf("[SPK] Starting STREAMING playback (ID: %lu)\n", stats.spkPlaybackCount + 1);
        Serial.printf("[SPK] Total chunks: %d (streaming mode)\n", totalChunks);
        
        // Free old buffer if exists
        if (speaker.audioBuffer != nullptr) {
            free(speaker.audioBuffer);
            speaker.audioBuffer = nullptr;
        }
        
        // Allocate small buffer for streaming
        speaker.audioBuffer = (uint8_t*)malloc(CHUNK_BUFFER_SIZE);
        if (speaker.audioBuffer == nullptr) {
            Serial.printf("[SPK] ❌ Failed to allocate %d bytes!\n", CHUNK_BUFFER_SIZE);
            Serial.printf("[SPK] Free heap: %s\n", formatBytes(ESP.getFreeHeap()).c_str());
            return;
        }
        
        speaker.bufferSize = CHUNK_BUFFER_SIZE;
        speaker.totalChunks = totalChunks;
        speaker.currentChunk = -1;
        
        Serial.printf("[SPK] ✅ Streaming buffer ready: %s\n", formatBytes(speaker.bufferSize).c_str());
    }
    
    if (speaker.audioBuffer == nullptr) {
        Serial.println("[SPK] ❌ Buffer allocation failed!");
        return;
    }
    
    // Decode base64 to buffer
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
        stats.spkDecodeErrors++;  // ✅ Track decode errors
        Serial.printf("[SPK] ❌ Base64 decode failed! Error: %d (Total errors: %lu)\n", 
                     result, stats.spkDecodeErrors);
        return;
    }
    
    speaker.bufferUsed = decodedLength;
    speaker.currentChunk = chunkNumber;
    stats.spkChunksReceived++;
    stats.spkLastPlayTime = millis();  // ✅ Track play activity
    
    // ✅ DETAILED LOGGING: Show every chunk received with timestamp
    unsigned long now = millis();
    if (DEBUG_SPEAKER) {
        Serial.printf("[SPK] ✅ Chunk %d/%d at %lu ms (%d bytes) [Gap: %lu ms]\n",
                     chunkNumber + 1, totalChunks, now, decodedLength,
                     (chunkNumber == 0) ? 0 : (now - stats.spkLastPlayTime));
    }
    
    // Progress every 10 chunks
    if ((chunkNumber + 1) % 10 == 0 || (chunkNumber + 1) == totalChunks) {
        int progress = ((chunkNumber + 1) * 100) / totalChunks;
        Serial.printf("[SPK] 📥 Streaming: %d%% (%d/%d chunks) - Connection: %s\n",
                     progress, chunkNumber + 1, totalChunks, 
                     wsConnected ? "ALIVE ✅" : "DEAD ❌");
    }
    
    // ✅ PLAY THIS CHUNK - wait for playback task to copy to I2S DMA
    if (decodedLength > 0) {
        speaker.isPlaying = true;
        
        // Wait for playback task to write chunk to I2S DMA buffer
        // This typically takes ~50ms, then buffer is safe to reuse
        unsigned long waitStart = millis();
        unsigned long timeout = 200;  // 200ms timeout
        
        while (speaker.isPlaying && (millis() - waitStart) < timeout) {
            webSocket.loop();  // Keep connection alive!
            delay(5);
        }
        
        if (speaker.isPlaying) {
            Serial.printf("[SPK] ⚠️ Playback task didn't finish in %lu ms!\n", timeout);
            speaker.isPlaying = false;
        }
        
        if (DEBUG_SPEAKER && chunkNumber < 5) {
            Serial.printf("[SPK] Buffer copied to DMA in %lu ms, ready for next\n", 
                         millis() - waitStart);
        }
    }
    
    // Last chunk - cleanup
    if ((chunkNumber + 1) >= totalChunks) {
        Serial.println("\n╔════════════════════════════════════════════════════════════╗");
        Serial.println("║           ✅ TTS STREAMING COMPLETE                        ║");
        Serial.println("╚════════════════════════════════════════════════════════════╝");
        
        stats.spkPlaybackCount++;
        Serial.printf("[SPK] Playback #%lu finished (%d chunks)\n", 
                     stats.spkPlaybackCount, totalChunks);
        
        // ✅ Small delay to ensure last chunk plays, but keep WebSocket alive
        for (int i = 0; i < 20; i++) {  // 200ms total
            webSocket.loop();  // ✅ Keep connection alive!
            delay(10);
        }
        
        // Clear DMA
        i2s_zero_dma_buffer(I2S_NUM_1);
        
        // Free buffer
        if (speaker.audioBuffer != nullptr) {
            free(speaker.audioBuffer);
            speaker.audioBuffer = nullptr;
        }
        
        speaker.currentChunk = -1;
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
            Serial.println("\n[SPK-TASK] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            Serial.printf("[SPK-TASK] 🔊 PLAYBACK STARTING at %lu ms\n", millis());
            Serial.printf("[SPK-TASK] Buffer: %p\n", speaker.audioBuffer);
            Serial.printf("[SPK-TASK] Buffer size: %d bytes (%d samples)\n", 
                         speaker.bufferUsed, speaker.bufferUsed / 2);
            Serial.printf("[SPK-TASK] Volume gain: %.1fx\n", SPK_VOLUME_GAIN);
            
            // ✅ Apply volume gain to audio samples
            int16_t* audioSamples = (int16_t*)speaker.audioBuffer;
            size_t sampleCount = speaker.bufferUsed / sizeof(int16_t);
            
            Serial.printf("[SPK-TASK] Processing %d samples...\n", sampleCount);
            
            // Check first few samples BEFORE gain
            Serial.printf("[SPK-TASK] First 5 samples BEFORE gain: ");
            for (int i = 0; i < 5 && i < sampleCount; i++) {
                Serial.printf("%d ", audioSamples[i]);
            }
            Serial.println();
            
            int nonZeroSamples = 0;
            for (size_t i = 0; i < sampleCount; i++) {
                if (audioSamples[i] != 0) nonZeroSamples++;
                
                // Apply gain
                int32_t amplified = (int32_t)(audioSamples[i] * SPK_VOLUME_GAIN);
                
                // Clamp to prevent clipping/distortion
                if (amplified > 32767) amplified = 32767;
                if (amplified < -32768) amplified = -32768;
                
                audioSamples[i] = (int16_t)amplified;
            }
            
            Serial.printf("[SPK-TASK] Non-zero samples: %d/%d (%.1f%%)\n",
                         nonZeroSamples, sampleCount, 
                         (nonZeroSamples * 100.0) / sampleCount);
            
            // Check first few samples AFTER gain
            Serial.printf("[SPK-TASK] First 5 samples AFTER gain: ");
            for (int i = 0; i < 5 && i < sampleCount; i++) {
                Serial.printf("%d ", audioSamples[i]);
            }
            Serial.println();
            
            // Write amplified audio data to I2S
            Serial.printf("[SPK-TASK] Writing to I2S_NUM_1...\n");
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
                    Serial.printf("[SPK-TASK] ❌ I2S write error: %d at offset %d\n", 
                                 result, totalWritten);
                    break;
                }
                
                if (bytesWritten == 0) {
                    Serial.printf("[SPK-TASK] ⚠️  I2S wrote 0 bytes!\n");
                    break;
                }
                
                totalWritten += bytesWritten;
                stats.spkBytesPlayed += bytesWritten;
                
                // Progress for large chunks
                if (totalToWrite > 4096 && totalWritten % 2048 == 0) {
                    Serial.printf("[SPK-TASK]   Written: %d/%d bytes (%.1f%%)\n",
                                 totalWritten, totalToWrite, 
                                 (totalWritten * 100.0) / totalToWrite);
                }
            }
            
            Serial.printf("[SPK-TASK] ✅ I2S write complete: %d/%d bytes\n", 
                         totalWritten, totalToWrite);
            
            // Calculate duration just for logging
            unsigned long expectedDurationMs = (sampleCount * 1000) / AUDIO_SAMPLE_RATE;
            Serial.printf("[SPK-TASK] Audio duration: %lu ms (%d samples)\n", 
                         expectedDurationMs, sampleCount);
            
            // ✅ Set isPlaying = false IMMEDIATELY - don't wait!
            // I2S DMA buffer will play the audio in background
            Serial.println("[SPK-TASK] ✅ Chunk written to I2S DMA buffer");
            Serial.println("[SPK-TASK] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
            
            // Mark as done
            speaker.isPlaying = false;
            Serial.printf("[SPK-TASK] Set isPlaying = false at %lu ms\n", millis());
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
    stats.wsLastMessageTime = millis();  // ✅ Track last message time
    
    if (DEBUG_WEBSOCKET) {
        Serial.printf("\n[WS] ⬇️  Message #%lu received (%d bytes)\n",
                     stats.wsMessagesReceived, length);
    }
    
    // Parse JSON
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (error) {
        stats.wsErrors++;  // ✅ Track JSON errors
        Serial.printf("[WS] ❌ JSON parse error: %s (Total errors: %lu)\n", 
                     error.c_str(), stats.wsErrors);
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
// DIAGNOSTICS AND MONITORING
// ═════════════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────────────
// Print comprehensive system diagnostics
// ────────────────────────────────────────────────────────────────────────────
void printCompleteDiagnostics() {
    unsigned long now = millis();
    
    if (!DEBUG_DIAGNOSTICS || (now - stats.lastDiagnostics < DIAGNOSTICS_INTERVAL_MS)) {
        return;
    }
    
    stats.lastDiagnostics = now;
    stats.currentFreeHeap = ESP.getFreeHeap();
    
    // Track minimum free heap
    if (stats.minFreeHeap == 0 || stats.currentFreeHeap < stats.minFreeHeap) {
        stats.minFreeHeap = stats.currentFreeHeap;
    }
    
    Serial.println("\n╔════════════════════════════════════════════════════════════════════════╗");
    Serial.println("║                    COMPLETE SYSTEM DIAGNOSTICS                         ║");
    Serial.println("╠════════════════════════════════════════════════════════════════════════╣");
    
    // ═══ SYSTEM STATUS ═══
    Serial.println("║ ┌─ SYSTEM STATUS ─────────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Uptime:              %-44s │ ║\n", formatDuration(now / 1000).c_str());
    Serial.printf("║ │ Free Heap:           %-44s │ ║\n", formatBytes(stats.currentFreeHeap).c_str());
    Serial.printf("║ │ Min Free Heap:       %-44s │ ║\n", formatBytes(stats.minFreeHeap).c_str());
    Serial.printf("║ │ Heap Fragmentation:  %d%% %-40s │ ║\n", 
                 (int)(100 - (ESP.getMaxAllocHeap() * 100 / ESP.getFreeHeap())), "");
    Serial.printf("║ │ Largest Block:       %-44s │ ║\n", formatBytes(ESP.getMaxAllocHeap()).c_str());
    Serial.printf("║ │ CPU Frequency:       %d MHz %-36s │ ║\n", ESP.getCpuFreqMHz(), "");
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ WIFI STATUS ═══
    Serial.println("║ ┌─ WIFI STATUS ───────────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Status:              %-44s │ ║\n", 
                 WiFi.status() == WL_CONNECTED ? "CONNECTED ✅" : "DISCONNECTED ❌");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("║ │ SSID:                %-44s │ ║\n", WiFi.SSID().c_str());
        Serial.printf("║ │ IP Address:          %-44s │ ║\n", WiFi.localIP().toString().c_str());
        Serial.printf("║ │ Signal Strength:     %d dBm %-36s │ ║\n", WiFi.RSSI(), "");
        Serial.printf("║ │ MAC Address:         %-44s │ ║\n", WiFi.macAddress().c_str());
    }
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ WEBSOCKET STATUS ═══
    Serial.println("║ ┌─ WEBSOCKET STATUS ──────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Connection:          %-44s │ ║\n", 
                 wsConnected ? "CONNECTED ✅" : "DISCONNECTED ❌");
    Serial.printf("║ │ Registered:          %-44s │ ║\n", 
                 wsRegistered ? "YES ✅" : "NO ❌");
    Serial.printf("║ │ Messages Sent:       %-44lu │ ║\n", stats.wsMessagesSent);
    Serial.printf("║ │ Messages Received:   %-44lu │ ║\n", stats.wsMessagesReceived);
    Serial.printf("║ │ Reconnections:       %-44lu │ ║\n", stats.wsReconnections);
    Serial.printf("║ │ Errors:              %-44lu │ ║\n", stats.wsErrors);
    
    if (stats.wsLastConnectTime > 0) {
        unsigned long connectedDuration = (now - stats.wsLastConnectTime) / 1000;
        Serial.printf("║ │ Connected For:       %-44s │ ║\n", formatDuration(connectedDuration).c_str());
    }
    if (stats.wsLastMessageTime > 0) {
        unsigned long timeSinceMessage = (now - stats.wsLastMessageTime) / 1000;
        Serial.printf("║ │ Last Message:        %lu seconds ago %-28s │ ║\n", timeSinceMessage, "");
    }
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ MICROPHONE STATUS ═══
    Serial.println("║ ┌─ MICROPHONE STATUS ─────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Chunks Sent:         %-44lu │ ║\n", stats.micChunksSent);
    Serial.printf("║ │ Chunks Dropped:      %-44lu │ ║\n", stats.micChunksDropped);
    Serial.printf("║ │ Bytes Read:          %-44s │ ║\n", formatBytes(stats.micBytesRead).c_str());
    Serial.printf("║ │ Read Errors:         %-44lu │ ║\n", stats.micReadErrors);
    
    // Calculate drop rate
    if (stats.micChunksSent + stats.micChunksDropped > 0) {
        float dropRate = (stats.micChunksDropped * 100.0) / (stats.micChunksSent + stats.micChunksDropped);
        Serial.printf("║ │ Drop Rate:           %.2f%% %-39s │ ║\n", dropRate, "");
    }
    
    // Queue status
    int queueUsed = uxQueueMessagesWaiting(micSendQueue);
    float queuePercent = (queueUsed * 100.0f) / MIC_SEND_QUEUE_SIZE;
    Serial.printf("║ │ Queue Usage:         %d/%d (%.1f%%) %-28s │ ║\n", 
                 queueUsed, MIC_SEND_QUEUE_SIZE, queuePercent, "");
    
    if (stats.micLastReadTime > 0) {
        unsigned long timeSinceRead = (now - stats.micLastReadTime) / 1000;
        Serial.printf("║ │ Last Read:           %lu seconds ago %-28s │ ║\n", timeSinceRead, "");
    }
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ SPEAKER STATUS ═══
    Serial.println("║ ┌─ SPEAKER STATUS ────────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Playback Task:       %-44s │ ║\n",
                 speakerPlaybackTask != NULL ? "RUNNING ✅" : "STOPPED ❌");
    Serial.printf("║ │ Currently Playing:   %-44s │ ║\n", 
                 speaker.isPlaying ? "YES 🔊" : "NO");
    Serial.printf("║ │ Chunks Received:     %-44lu │ ║\n", stats.spkChunksReceived);
    Serial.printf("║ │ Bytes Played:        %-44s │ ║\n", formatBytes(stats.spkBytesPlayed).c_str());
    Serial.printf("║ │ Total Playbacks:     %-44lu │ ║\n", stats.spkPlaybackCount);
    Serial.printf("║ │ Decode Errors:       %-44lu │ ║\n", stats.spkDecodeErrors);
    
    // Current stream info
    if (speaker.audioBuffer != nullptr) {
        Serial.printf("║ │ Buffer Allocated:    %-44s │ ║\n", formatBytes(speaker.bufferSize).c_str());
        Serial.printf("║ │ Buffer Used:         %-44s │ ║\n", formatBytes(speaker.bufferUsed).c_str());
        Serial.printf("║ │ Current Chunk:       %d/%d %-38s │ ║\n", 
                     speaker.currentChunk + 1, speaker.totalChunks, "");
    } else {
        Serial.printf("║ │ Buffer Allocated:    %-44s │ ║\n", "None (idle)");
    }
    
    if (stats.spkLastPlayTime > 0) {
        unsigned long timeSincePlay = (now - stats.spkLastPlayTime) / 1000;
        Serial.printf("║ │ Last Playback:       %lu seconds ago %-28s │ ║\n", timeSincePlay, "");
    }
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ I2S STATUS ═══
    Serial.println("║ ┌─ I2S STATUS ────────────────────────────────────────────────────────┐ ║");
    Serial.printf("║ │ Microphone (I2S_0):  GPIO %d (WS), %d (SCK), %d (SD) %-12s │ ║\n",
                 I2S_MIC_WORD_SELECT, I2S_MIC_SERIAL_CLOCK, I2S_MIC_SERIAL_DATA, "");
    Serial.printf("║ │ Speaker (I2S_1):     GPIO %d (LRC), %d (BCLK), %d (DIN) %-11s │ ║\n",
                 I2S_SPK_WORD_SELECT, I2S_SPK_SERIAL_CLOCK, I2S_SPK_SERIAL_DATA, "");
    Serial.printf("║ │ Sample Rate:         %d Hz %-36s │ ║\n", AUDIO_SAMPLE_RATE, "");
    Serial.printf("║ │ Mic Gain:            %.1fx %-40s │ ║\n", MIC_GAIN_MULTIPLIER, "");
    Serial.printf("║ │ Speaker Volume:      %.1fx %-40s │ ║\n", SPK_VOLUME_GAIN, "");
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    // ═══ HEALTH INDICATORS ═══
    Serial.println("║ ┌─ HEALTH INDICATORS ─────────────────────────────────────────────────┐ ║");
    
    // Check WebSocket health
    bool wsHealthy = wsConnected && wsRegistered && 
                     (now - stats.wsLastMessageTime < 60000 || stats.wsMessagesReceived == 0);
    Serial.printf("║ │ WebSocket Health:    %-44s │ ║\n", 
                 wsHealthy ? "HEALTHY ✅" : "DEGRADED ⚠️");
    
    // Check microphone health
    bool micHealthy = (stats.micChunksDropped * 100 / max(1UL, stats.micChunksSent + stats.micChunksDropped)) < 5;
    Serial.printf("║ │ Microphone Health:   %-44s │ ║\n", 
                 micHealthy ? "HEALTHY ✅" : "DEGRADED ⚠️");
    
    // Check memory health
    bool memHealthy = stats.currentFreeHeap > 50000;
    Serial.printf("║ │ Memory Health:       %-44s │ ║\n", 
                 memHealthy ? "HEALTHY ✅" : "LOW ⚠️");
    
    // Overall system health
    bool systemHealthy = wsHealthy && micHealthy && memHealthy;
    Serial.printf("║ │ Overall System:      %-44s │ ║\n", 
                 systemHealthy ? "HEALTHY ✅" : "DEGRADED ⚠️");
    
    Serial.println("║ └──────────────────────────────────────────────────────────────────────┘ ║");
    
    Serial.println("╚════════════════════════════════════════════════════════════════════════╝\n");
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
    
    // ⚠️ BROWNOUT DETECTOR - Only disable for testing!
    #if DISABLE_BROWNOUT_DETECTOR
    Serial.println("⚠️  WARNING: Brownout detector DISABLED!");
    Serial.println("⚠️  This is NOT recommended for production!");
    Serial.println("⚠️  Fix your power supply instead!");
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    delay(1000);
    #endif
    
    // Print startup banner
    Serial.println("\n\n");
    printDivider('═');
    Serial.println("║                                                                          ║");
    Serial.println("║              ESP32 COMPLETE VOICE ASSISTANT - PRODUCTION                 ║");
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
    
    // ✅ CRITICAL FIX: Initialize MICROPHONE FIRST to avoid pin conflicts!
    if (!initializeMicrophone()) {
        Serial.println("\n❌ FATAL: Microphone initialization failed!");
        Serial.println("System halted.");
        while (1) delay(1000);
    }
    delay(100);
    
    if (!initializeSpeaker()) {
        Serial.println("\n❌ FATAL: Speaker initialization failed!");
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
        
        // ✅ Extra delay to reduce power consumption during WiFi connect
        delay(200);
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
    stats.lastDiagnostics = millis();
    stats.minFreeHeap = ESP.getFreeHeap();  // ✅ Initialize min heap
    stats.currentFreeHeap = ESP.getFreeHeap();
}

// ═════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    // ✅ CRITICAL: Service WebSocket FIRST, ALWAYS!
    webSocket.loop();
    
    // Check if WebSocket is connected
    if (!wsConnected) {
        webSocket.loop();  // ✅ Call again to help connection
        delay(10);
        return;
    }
    
    // Pause microphone while speaker is playing to avoid feedback
    if (speaker.isPlaying) {
        webSocket.loop();  // ✅ Keep calling even when not recording
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
        stats.micReadErrors++;  // ✅ Track errors
        static unsigned long lastErrorTime = 0;
        if (millis() - lastErrorTime > 1000) {
            Serial.printf("[MIC] ⚠️  I2S read error: %d (Total errors: %lu)\n", 
                         result, stats.micReadErrors);
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
    
    stats.micLastReadTime = millis();  // ✅ Track last successful read
    
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
    
    // ✅ Print comprehensive diagnostics
    printCompleteDiagnostics();
    
    // Small delay to prevent tight looping
    delay(10);
}
