/*
  HK Onyx Studio 5 - power + sound test (ESP32, Arduino core 2.0.x)
  Sequence:
   1. Hold Power_Off_Ready HIGH immediately (keeps SLG alive -> booster + 3V3 stay on)
   2. Wait for 10V rail, scan I2C (blink RED_LED once per device found)
   3. Release DSP_PDN, start I2S
   4. AMP_PWR_CTRL on (Q18 switch -> amp supply), then unmute
   5. Loop: soft tones, WHITE_LED blinks with each tone
  Serial is NOT used (TX0/RX0 are WHITE_LED / AMP_PWR_CTRL).
  /SHDN, IO_INT, Power_On_Off, POWER_ON/OFF_OP are left as inputs (untouched).
*/
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/i2s.h"

// ---------------- Pins ----------------
#define PIN_SDA            21
#define PIN_SCL            22
#define PIN_I2S_BCLK       25
#define PIN_I2S_DOUT       26
#define PIN_I2S_LRCLK      14
#define PIN_I2S_DIN        27
#define PIN_PWR_OFF_READY   4   // HIGH = stay on (measured)
#define PIN_DSP_PDN         5
#define PIN_MUTE           19
#define PIN_RED_LED        12
#define PIN_WHITE_LED       1   // TX0
#define PIN_AMP_PWR         3   // RX0
#define PIN_SHDN            2   // untouched
#define PIN_PWR_ON_OFF     17   // untouched
#define PIN_PWR_ONOFF_OP   16   // untouched

// ---------------- Polarity / config ----------------
#define AMP_ON    HIGH   // AMP_PWR_CTRL high -> NPN -> P-FET on (from schematic)
#define AMP_OFF   LOW
#define MUTE_ON   HIGH   // MUTE high -> Q17 pulls /MUTE low -> muted (from schematic)
#define MUTE_OFF  LOW
#define PDN_RUN   HIGH   // AK7755 PDN: low = power down
#define LED_ON    HIGH   // flip if LEDs are active-low

#define I2S_ROLE_MASTER   1       // 1: ESP32 drives BCLK/LRCLK. 0: DSP is master
#define SAMPLE_RATE       48000   // DSP crystal is 12.288 MHz
#define AMPLITUDE         3000    // out of 32767 - keep LOW for first test

#define RAIL_WAIT_MS      1500
#define AMP_SETTLE_MS     500

static void led(int pin, bool on) { digitalWrite(pin, on ? LED_ON : !LED_ON); }

static void blinkN(int pin, int n) {
  for (int i = 0; i < n; i++) { led(pin, true); delay(200); led(pin, false); delay(200); }
  delay(600);
}

static int i2cScan() {
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) found++;
  }
  return found;
}

static void i2sStart() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)((I2S_ROLE_MASTER ? I2S_MODE_MASTER : I2S_MODE_SLAVE) | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRCLK;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &pins);
}

static void playTone(float freq, int ms) {
  const int N = 256;
  int16_t buf[N * 2];
  static float phase = 0;
  float step = 2.0f * PI * freq / SAMPLE_RATE;
  uint32_t total = (uint32_t)SAMPLE_RATE * ms / 1000;
  uint32_t done = 0;
  while (done < total) {
    for (int i = 0; i < N; i++) {
      // short fade in/out to avoid clicks
      float env = 1.0f;
      uint32_t pos = done + i;
      if (pos < 2400) env = pos / 2400.0f;
      else if (pos > total - 2400) env = (total > pos) ? (total - pos) / 2400.0f : 0;
      int16_t s = (int16_t)(sinf(phase) * AMPLITUDE * env);
      phase += step; if (phase > 2 * PI) phase -= 2 * PI;
      buf[2 * i] = s; buf[2 * i + 1] = s;
    }
    size_t w;
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, portMAX_DELAY);
    done += N;
  }
}

void setup() {
  // 1. Keep the SLG alive FIRST (must be within ~900 ms of wake)
  pinMode(PIN_PWR_OFF_READY, OUTPUT);
  digitalWrite(PIN_PWR_OFF_READY, HIGH);

  // Safe states for everything else
  pinMode(PIN_MUTE, OUTPUT);     digitalWrite(PIN_MUTE, MUTE_ON);
  pinMode(PIN_AMP_PWR, OUTPUT);  digitalWrite(PIN_AMP_PWR, AMP_OFF);
  pinMode(PIN_DSP_PDN, OUTPUT);  digitalWrite(PIN_DSP_PDN, !PDN_RUN);
  pinMode(PIN_RED_LED, OUTPUT);  led(PIN_RED_LED, false);
  pinMode(PIN_WHITE_LED, OUTPUT); led(PIN_WHITE_LED, false);
  pinMode(PIN_SHDN, INPUT);
  pinMode(PIN_PWR_ON_OFF, INPUT);
  pinMode(PIN_PWR_ONOFF_OP, INPUT);

  // 2. Let booster / 3V3 settle, then scan I2C
  delay(RAIL_WAIT_MS);
  Wire.begin(PIN_SDA, PIN_SCL);
  int n = i2cScan();
  blinkN(PIN_RED_LED, n);          // 0 blinks = nothing found

  // 3. Wake DSP (reset pulse) and start I2S
  digitalWrite(PIN_DSP_PDN, !PDN_RUN); delay(20);
  digitalWrite(PIN_DSP_PDN, PDN_RUN);  delay(200);
  i2sStart();
  delay(200);

  // 4. Amp supply on, settle, unmute
  digitalWrite(PIN_AMP_PWR, AMP_ON);
  delay(AMP_SETTLE_MS);
  digitalWrite(PIN_MUTE, MUTE_OFF);
  led(PIN_RED_LED, true);          // solid red = reached tone phase
}

void loop() {
  const float notes[] = {440, 660, 880, 660};
  for (float f : notes) {
    led(PIN_WHITE_LED, true);
    playTone(f, 700);
    led(PIN_WHITE_LED, false);
    delay(300);
  }
  delay(1500);
}
