/*
 * ESP32 Speaker Test - Simple Beep
 * 
 * This will generate a 1000 Hz beep for 2 seconds
 * If you hear it, your wiring is correct!
 */

#include <driver/i2s.h>

#define SPK_LRC  14
#define SPK_BCLK 12
#define SPK_DIN  13

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("╔════════════════════════════════════╗");
  Serial.println("║    ESP32 Speaker Test - BEEP      ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  // Configure I2S
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 8000,
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
  
  Serial.printf("[I2S] Initialized on GPIO %d, %d, %d\n", SPK_LRC, SPK_BCLK, SPK_DIN);
  Serial.println("[I2S] Speaker should be connected to MAX98357A");
  Serial.println();
  
  delay(1000);
  
  // Generate 1000 Hz beep for 2 seconds
  Serial.println("[TEST] 🔊 Generating 1000 Hz beep...");
  Serial.println("[TEST] Listen for a high-pitched tone!");
  Serial.println();
  
  int16_t samples[16];  // One cycle of 1000 Hz at 16000 Hz
  
  // Generate sine wave samples for 1000 Hz
  for (int i = 0; i < 16; i++) {
    float angle = (2.0 * PI * i) / 16.0;
    samples[i] = (int16_t)(sin(angle) * 15000);  // Amplitude 15000 (loud!)
  }
  
  // Play for 2 seconds (16000 samples)
  for (int i = 0; i < 1000; i++) {  // 1000 cycles = 2 seconds at 8000 Hz
    size_t written;
    i2s_write(I2S_NUM_1, samples, sizeof(samples), &written, portMAX_DELAY);
    
    if (i % 100 == 0) {
      Serial.printf("[TEST] Playing... %d%%\n", (i * 100) / 1000);
    }
  }
  
  i2s_zero_dma_buffer(I2S_NUM_1);
  
  Serial.println();
  Serial.println("╔════════════════════════════════════╗");
  Serial.println("║       ✅ TEST COMPLETE            ║");
  Serial.println("╚════════════════════════════════════╝");
  Serial.println();
  Serial.println("DID YOU HEAR A BEEP?");
  Serial.println();
  Serial.println("YES → Wiring is correct! ✅");
  Serial.println("      Problem is with TTS audio data");
  Serial.println();
  Serial.println("NO  → Check wiring: ❌");
  Serial.println("      - ESP32 GPIO 14 → MAX98357A LRC");
  Serial.println("      - ESP32 GPIO 12 → MAX98357A BCLK");
  Serial.println("      - ESP32 GPIO 13 → MAX98357A DIN");
  Serial.println("      - ESP32 5V → MAX98357A VIN");
  Serial.println("      - ESP32 GND → MAX98357A GND");
  Serial.println("      - MAX98357A + → Speaker +");
  Serial.println("      - MAX98357A - → Speaker -");
  Serial.println();
  Serial.println("Repeating test in 5 seconds...");
}

void loop() {
  delay(5000);
  
  // Repeat beep every 5 seconds
  Serial.println("\n[TEST] 🔊 BEEP!");
  
  int16_t samples[16];
  for (int i = 0; i < 16; i++) {
    float angle = (2.0 * PI * i) / 16.0;
    samples[i] = (int16_t)(sin(angle) * 15000);
  }
  
  for (int i = 0; i < 1000; i++) {
    size_t written;
    i2s_write(I2S_NUM_1, samples, sizeof(samples), &written, portMAX_DELAY);
  }
  
  i2s_zero_dma_buffer(I2S_NUM_1);
}
