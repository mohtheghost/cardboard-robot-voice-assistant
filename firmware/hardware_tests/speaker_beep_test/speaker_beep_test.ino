/*
 * ESP32 SPEAKER TEST - Continuous Beep
 * Simple code to test MAX98357A speaker connection
 * 
 * Wiring:
 * ESP32 GPIO 12 → MAX98357A BCLK
 * ESP32 GPIO 14 → MAX98357A LRC
 * ESP32 GPIO 13 → MAX98357A DIN
 * ESP32 GND → MAX98357A GND
 * ESP32 5V → MAX98357A VIN
 */

#include <driver/i2s.h>

// I2S Speaker Pins (MAX98357A)
#define I2S_SPK_BCLK    12    // Bit Clock
#define I2S_SPK_LRC     14    // Left/Right Clock (Word Select)
#define I2S_SPK_DIN     13    // Data In

// Audio Settings
#define SAMPLE_RATE     8000
#define BEEP_FREQUENCY  800   // Hz
#define BEEP_DURATION   200   // milliseconds
#define BEEP_PAUSE      300   // milliseconds between beeps
#define BEEP_VOLUME     0.2f  // Volume (0.0 to 1.0)

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════════════╗");
  Serial.println("║   ESP32 SPEAKER TEST - Continuous Beep   ║");
  Serial.println("╚════════════════════════════════════════════╝\n");
  
  // Initialize I2S for speaker
  i2s_config_t i2s_config = {
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
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
  
  // Install I2S driver
  esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S driver install failed: %d\n", err);
    while(1) delay(1000);
  }
  
  // Set I2S pins
  err = i2s_set_pin(I2S_NUM_1, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S set pin failed: %d\n", err);
    while(1) delay(1000);
  }
  
  // Start I2S
  err = i2s_start(I2S_NUM_1);
  if (err != ESP_OK) {
    Serial.printf("❌ I2S start failed: %d\n", err);
    while(1) delay(1000);
  }
  
  Serial.println("✅ Speaker initialized successfully!");
  Serial.printf("📊 Sample rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("🎵 Beep frequency: %d Hz\n", BEEP_FREQUENCY);
  Serial.printf("⏱️  Beep duration: %d ms\n", BEEP_DURATION);
  Serial.printf("⏸️  Pause between beeps: %d ms\n", BEEP_PAUSE);
  Serial.printf("🔊 Volume: %.0f%%\n", BEEP_VOLUME * 100);
  Serial.println("\n🔔 Starting continuous beep...\n");
}

void loop() {
  // Generate and play beep
  playBeep(BEEP_FREQUENCY, BEEP_DURATION, BEEP_VOLUME);
  
  // Pause between beeps
  delay(BEEP_PAUSE);
  
  // Print status every 10 beeps
  static int beepCount = 0;
  beepCount++;
  if (beepCount % 10 == 0) {
    Serial.printf("🔔 Beep count: %d\n", beepCount);
  }
}

void playBeep(int frequency, int durationMs, float volume) {
  // Calculate number of samples
  size_t numSamples = (SAMPLE_RATE * durationMs) / 1000;
  
  // Allocate buffer for beep samples
  int16_t* beepBuffer = (int16_t*)malloc(numSamples * sizeof(int16_t));
  if (!beepBuffer) {
    Serial.println("❌ Memory allocation failed!");
    return;
  }
  
  // Generate sine wave with envelope
  float phase = 0.0f;
  float phaseIncrement = (TWO_PI * frequency) / SAMPLE_RATE;
  
  for (size_t i = 0; i < numSamples; i++) {
    // Apply fade in/out envelope to avoid clicks
    float envelope = 1.0f;
    
    // Fade in (first 5ms)
    if (i < (SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)i / (SAMPLE_RATE * 5 / 1000);
    }
    // Fade out (last 5ms)
    else if (i > numSamples - (SAMPLE_RATE * 5 / 1000)) {
      envelope = (float)(numSamples - i) / (SAMPLE_RATE * 5 / 1000);
    }
    
    // Generate sine wave sample
    float sample = sinf(phase) * envelope * volume * 32767.0f;
    beepBuffer[i] = (int16_t)sample;
    
    // Increment phase
    phase += phaseIncrement;
    if (phase >= TWO_PI) {
      phase -= TWO_PI;
    }
  }
  
  // Write to I2S
  size_t totalWritten = 0;
  size_t totalBytes = numSamples * sizeof(int16_t);
  
  while (totalWritten < totalBytes) {
    size_t toWrite = min((size_t)2048, totalBytes - totalWritten);
    size_t written = 0;
    
    esp_err_t result = i2s_write(
      I2S_NUM_1,
      (uint8_t*)beepBuffer + totalWritten,
      toWrite,
      &written,
      portMAX_DELAY
    );
    
    if (result != ESP_OK) {
      Serial.printf("❌ I2S write error: %d\n", result);
      break;
    }
    
    totalWritten += written;
  }
  
  // Clean up
  free(beepBuffer);
}
