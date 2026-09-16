/*
 * ╔══════════════════════════════════════════════════════════════════════════╗
 * ║                                                                          ║
 * ║          ESP32 VOICE ASSISTANT - WITH REMOTE DIAGNOSTICS                ║
 * ║                                                                          ║
 * ║  Features:                                                               ║
 * ║  ✅ Dual I2S - Separate microphone and speaker                          ║
 * ║  ✅ WebSocket communication                                             ║
 * ║  ✅ Remote diagnostics - Monitor from PC!                               ║
 * ║  ✅ Optimized for stability and performance                             ║
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

// VPS Server
#define VPS_HOST "YOUR_SERVER_IP"
#define VPS_PORT 8080
#define VPS_PATH "/"

// WebSocket
#define WS_HEARTBEAT_INTERVAL_MS 5000
#define WS_HEARTBEAT_TIMEOUT_MS 10000
#define WS_HEARTBEAT_PONG_RETRIES 5

// I2S Pins - Microphone
#define I2S_MIC_SCK   25
#define I2S_MIC_WS    27
#define I2S_MIC_SD    18

// I2S Pins - Speaker
#define I2S_SPK_BCLK  12
#define I2S_SPK_LRC   14
#define I2S_SPK_DIN   13

// Audio
#define AUDIO_SAMPLE_RATE 8000
#define MIC_GAIN          2.0f
#define SPK_GAIN          0.5f  // ✅ Reduced to prevent brownout!

// Diagnostics
#define ENABLE_DIAGNOSTICS    true
#define DIAG_BUFFER_SIZE      256

// ═════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ═════════════════════════════════════════════════════════════════════════════

WebSocketsClient webSocket;
volatile bool wsConnected = false;
volatile bool wsRegistered = false;

QueueHandle_t micQueue;
TaskHandle_t speakerTask = nullptr;

struct {
    bool playing;
    uint8_t* buffer;
    size_t size;
    size_t used;
    int currentChunk;
    int totalChunks;
} speaker = {false, nullptr, 0, 0, 0, 0};

// Diagnostics
#if ENABLE_DIAGNOSTICS
char diagBuf[DIAG_BUFFER_SIZE];
size_t diagPos = 0;

void sendDiag() {
    if (diagPos == 0 || !wsConnected || !wsRegistered) return;
    
    diagBuf[diagPos] = '\0';
    String msg = "{\"target\":\"pc\",\"type\":\"diagnostic\",\"data\":\"";
    
    for (size_t i = 0; i < diagPos; i++) {
        char c = diagBuf[i];
        if (c == '"') msg += "\\\"";
        else if (c == '\\') msg += "\\\\";
        else if (c == '\n') msg += "\\n";
        else if (c == '\r') msg += "\\r";
        else if (c >= 32 && c <= 126) msg += c;
    }
    
    msg += "\"}";
    webSocket.sendTXT(msg);
    diagPos = 0;
}

void addDiag(const char* text, bool isError = false) {
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (diagPos < DIAG_BUFFER_SIZE - 1) {
            diagBuf[diagPos++] = text[i];
        } else {
            sendDiag();
            if (diagPos < DIAG_BUFFER_SIZE - 1) diagBuf[diagPos++] = text[i];
        }
    }
    
    if (text[len-1] == '\n' || isError || diagPos >= DIAG_BUFFER_SIZE - 20) {
        if (wsConnected && wsRegistered) sendDiag();
    }
}

#define DIAG(fmt, ...) do { \
    char __b[200]; \
    snprintf(__b, sizeof(__b), fmt, __VA_ARGS__); \
    Serial.print(__b); \
    addDiag(__b); \
} while(0)

#define DIAG_ERR(fmt, ...) do { \
    char __b[200]; \
    snprintf(__b, sizeof(__b), fmt, __VA_ARGS__); \
    Serial.print(__b); \
    addDiag(__b, true); \
} while(0)

#else
#define DIAG(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define DIAG_ERR(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
void sendDiag() {}
#endif

// ═════════════════════════════════════════════════════════════════════════════
// I2S INIT
// ═════════════════════════════════════════════════════════════════════════════

bool initSpeaker() {
    Serial.println("[SPK] Initializing...");
    
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = AUDIO_SAMPLE_RATE,
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
        .bck_io_num = I2S_SPK_BCLK,
        .ws_io_num = I2S_SPK_LRC,
        .data_out_num = I2S_SPK_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    
    if (i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL) != ESP_OK) {
        DIAG_ERR("[SPK] Driver install failed\n");
        return false;
    }
    
    if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
        DIAG_ERR("[SPK] Pin config failed\n");
        return false;
    }
    
    i2s_start(I2S_NUM_1);
    Serial.println("[SPK] ✅ Ready");
    return true;
}

bool initMic() {
    Serial.println("[MIC] Initializing...");
    
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = AUDIO_SAMPLE_RATE,
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
        .bck_io_num = I2S_MIC_SCK,
        .ws_io_num = I2S_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_MIC_SD
    };
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
        DIAG_ERR("[MIC] Driver install failed\n");
        return false;
    }
    
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        DIAG_ERR("[MIC] Pin config failed\n");
        return false;
    }
    
    i2s_start(I2S_NUM_0);
    Serial.println("[MIC] ✅ Ready");
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// SPEAKER TASK
// ═════════════════════════════════════════════════════════════════════════════

void speakerPlaybackTask(void* param) {
    while (true) {
        if (speaker.playing && speaker.buffer && speaker.used > 0) {
            // Apply gain
            int16_t* samples = (int16_t*)speaker.buffer;
            size_t count = speaker.used / 2;
            
            for (size_t i = 0; i < count; i++) {
                int32_t amp = (int32_t)(samples[i] * SPK_GAIN);
                if (amp > 32767) amp = 32767;
                if (amp < -32768) amp = -32768;
                samples[i] = (int16_t)amp;
            }
            
            // Write to I2S
            size_t written = 0;
            i2s_write(I2S_NUM_1, speaker.buffer, speaker.used, &written, portMAX_DELAY);
            
            speaker.playing = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TTS HANDLER
// ═════════════════════════════════════════════════════════════════════════════

void handleTTS(JsonDocument& doc) {
    if (!doc.containsKey("chunk") || !doc.containsKey("total") || !doc.containsKey("data")) {
        DIAG_ERR("[TTS] Missing fields\n");
        return;
    }
    
    int chunk = doc["chunk"];
    int total = doc["total"];
    const char* b64 = doc["data"];
    
    if (!b64 || strlen(b64) == 0) {
        DIAG_ERR("[TTS] Empty data in chunk %d\n", chunk);
        return;
    }
    
    // Allocate buffer on first chunk
    if (speaker.buffer == nullptr) {
        speaker.buffer = (uint8_t*)malloc(8192);
        if (!speaker.buffer) {
            DIAG_ERR("[TTS] Alloc failed! Heap: %d\n", ESP.getFreeHeap());
            return;
        }
        speaker.size = 8192;
        speaker.currentChunk = -1;
        speaker.totalChunks = total;
        
        DIAG("[DIAG] TTS start: %d chunks, first=%d\n", total, chunk);
    }
    
    // Decode base64
    size_t decoded = 0;
    int result = mbedtls_base64_decode(speaker.buffer, speaker.size, &decoded,
                                       (const unsigned char*)b64, strlen(b64));
    
    if (result != 0) {
        DIAG_ERR("[TTS] Decode error %d on chunk %d\n", result, chunk);
        return;
    }
    
    speaker.used = decoded;
    speaker.currentChunk = chunk;
    
    // Progress
    if ((chunk + 1) % 50 == 0 || (chunk + 1) == total) {
        int pct = ((chunk + 1) * 100) / total;
        Serial.printf("[TTS] %d%% (%d/%d)\n", pct, chunk + 1, total);
    }
    
    // Play
    if (speaker.used > 0) {
        speaker.playing = true;
        
        unsigned long start = millis();
        while (speaker.playing && (millis() - start < 5000)) {
            webSocket.loop();
            delay(1);
        }
        
        if (speaker.playing) {
            DIAG_ERR("[TTS] Timeout on chunk %d\n", chunk);
            speaker.playing = false;
        }
    }
    
    // Cleanup on last chunk
    if ((chunk + 1) >= total) {
        DIAG("[DIAG] TTS complete: %d chunks\n", total);
        i2s_zero_dma_buffer(I2S_NUM_1);
        
        if (speaker.buffer) {
            free(speaker.buffer);
            speaker.buffer = nullptr;
        }
        speaker.used = 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// WEBSOCKET
// ═════════════════════════════════════════════════════════════════════════════

void handleWSMessage(char* payload, size_t length) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload, length)) {
        DIAG_ERR("[WS] JSON parse error\n");
        return;
    }
    
    const char* type = doc["type"];
    if (!type) return;
    
    if (strcmp(type, "tts") == 0) {
        handleTTS(doc);
    }
}

void onWSEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] ❌ DISCONNECTED");
            DIAG("[DIAG] WS disconnected at %lu ms\n", millis());
            wsConnected = false;
            wsRegistered = false;
            break;
            
        case WStype_CONNECTED:
            Serial.println("[WS] ✅ CONNECTED");
            DIAG("[DIAG] WS connected at %lu ms\n", millis());
            wsConnected = true;
            
            webSocket.sendTXT("{\"id\":\"esp32\"}");
            wsRegistered = true;
            Serial.println("[WS] Registered");
            break;
            
        case WStype_TEXT:
            handleWSMessage((char*)payload, length);
            break;
            
        case WStype_ERROR:
            DIAG_ERR("[WS] Error!\n");
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// MICROPHONE (simplified for now)
// ═════════════════════════════════════════════════════════════════════════════

void sendMicAudio(int16_t* samples, size_t count) {
    if (!wsConnected || !wsRegistered) return;
    
    // Base64 encode
    size_t pcmBytes = count * 2;
    size_t b64MaxLen = ((pcmBytes + 2) / 3) * 4 + 4;
    uint8_t* b64Buf = (uint8_t*)malloc(b64MaxLen);
    if (!b64Buf) return;
    
    size_t b64Len = 0;
    mbedtls_base64_encode(b64Buf, b64MaxLen, &b64Len, (uint8_t*)samples, pcmBytes);
    b64Buf[b64Len] = '\0';
    
    // Build JSON
    String msg = "{\"target\":\"pc\",\"type\":\"audio\",\"data\":\"";
    msg += (char*)b64Buf;
    msg += "\"}";
    
    webSocket.sendTXT(msg);
    free(b64Buf);
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n╔═══════════════════════════════════════╗");
    Serial.println("║  ESP32 Voice Assistant + Diagnostics  ║");
    Serial.println("╚═══════════════════════════════════════╝\n");
    
    // Init I2S
    if (!initSpeaker() || !initMic()) {
        Serial.println("❌ I2S init failed!");
        while(1) delay(1000);
    }
    
    // WiFi
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.printf("\n[WiFi] ✅ Connected: %s\n", WiFi.localIP().toString().c_str());
    DIAG("[DIAG] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    
    // WebSocket
    webSocket.begin(VPS_HOST, VPS_PORT, VPS_PATH);
    webSocket.onEvent(onWSEvent);
    webSocket.setReconnectInterval(3000);
    webSocket.enableHeartbeat(WS_HEARTBEAT_INTERVAL_MS, WS_HEARTBEAT_TIMEOUT_MS, 
                              WS_HEARTBEAT_PONG_RETRIES);
    
    Serial.println("[WS] Client configured");
    
    // Create speaker task
    xTaskCreatePinnedToCore(speakerPlaybackTask, "Speaker", 4096, NULL, 2, &speakerTask, 0);
    
    Serial.println("\n✅ System ready!");
    DIAG("[DIAG] System initialized at %lu ms\n", millis());
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
    webSocket.loop();
    
    if (!wsConnected || speaker.playing) {
        delay(5);
        return;
    }
    
    // Read mic (simplified - just read and send periodically)
    static int32_t micBuf[512];
    static int16_t pcmBuf[512];
    size_t bytesRead = 0;
    
    i2s_read(I2S_NUM_0, micBuf, sizeof(micBuf), &bytesRead, pdMS_TO_TICKS(100));
    
    if (bytesRead > 0) {
        size_t samples = bytesRead / 4;
        for (size_t i = 0; i < samples; i++) {
            int32_t s = micBuf[i] >> 14;
            s = (int32_t)(s * MIC_GAIN);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            pcmBuf[i] = (int16_t)s;
        }
        
        // Send every 6 reads (batching)
        static int readCount = 0;
        if (++readCount >= 6) {
            sendMicAudio(pcmBuf, samples);
            readCount = 0;
        }
    }
}
