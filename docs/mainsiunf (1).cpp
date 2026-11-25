/*
  HK Onyx Studio 5 - power + sound test (ESP32, Arduino core 2.0.x)
  Now with AK7755 initialisation per datasheet 014006643-E-00.

  Sequence:
   1. Hold Power_Off_Ready HIGH immediately (keeps SLG alive -> booster + 3V3 stay on)
   2. Wait for 10V rail, release DSP_PDN (PDN L->H), wait >=1 ms  [datasheet p.52/56]
   3. Scan I2C (AK7755 only answers after PDN release), blink RED_LED once per device
   4. Program AK7755 control registers while in CLOCK RESET (CKRESETN=0)
   5. Start I2S (BICK/LRCK must be running before clock reset release)
   6. Release clock reset (CKRESETN=1), wait for PLL (10 ms), release CODEC/DSP reset
   7. AMP_PWR_CTRL on (Q18 switch -> amp supply), unmute (MUTE pin + DAC soft mute)
   8. Loop: soft tones, WHITE_LED blinks with each tone

  No DSP program is downloaded: the DAC is fed directly from SDIN1 (CONT08 SELDAI=3),
  bypassing the DSP, so a plain I2S stream is enough to get sound.

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

#define I2S_ROLE_MASTER   1       // 1: ESP32 drives BCLK/LRCLK (AK7755 CKM3, PLL locks to BICK)
                                  // 0: AK7755 is master from its 12.288 MHz crystal (CKM0)
#define SAMPLE_RATE       48000   // DFS=5 (48 kHz). DSP crystal is 12.288 MHz
#define AMPLITUDE         3000    // out of 32767 - keep LOW for first test

#define RAIL_WAIT_MS      1500
#define AMP_SETTLE_MS     500

// ---------------- AK7755 settings ----------------
// I2C slave address: 0011 00 CAD  (I2CSEL=H, EXTEEP=L)  [p.81]
#define AK_ADDR_CAD_L     0x18
#define AK_ADDR_CAD_H     0x19

// Which SDIN pin the ESP32 DOUT is wired to: 3 = SDIN1 (pin 5), 2 = SDIN2 (pin 4)
// -> CONT08 SELDAI[1:0]  [p.39]. Change to 2 if I2S data goes to SDIN2.
#define AK_DAC_SRC        3

// Line-out volume, CONT13/14 LOVOLx[3:0]: 0=mute, 1=-28dB ... 0xF=0dB (2 dB steps) [p.47-48]
// Gain must not be changed while running (pop), so it is set during reset.
#define AK_LINEOUT_VOL    0xC     // -6 dB

#if I2S_ROLE_MASTER
  #define AK_CKM          3       // slave, BICK+LRCK are the clock source (XTI not used) [p.27]
  #define AK_BICKLRCK_OE  0x00    // CONT0A: BICK/LRCK are inputs
#else
  #define AK_CKM          0       // master, XTI = 12.288 MHz crystal [p.26]
  #define AK_BICKLRCK_OE  0x60    // CONT0A: BICKOE | LRCKOE
#endif
#define AK_DFS            5       // 48 kHz [p.31]
#define AK_BITFS          0       // BICK = 64 fs [p.32]
#define AK_LRIF           1       // I2S compatible [p.33]

// Register numbers. Write command code = 0xC0 | n, read command code = 0x40 | n  [p.30,82]
enum {
  CONT00 = 0x00, CONT01 = 0x01, CONT02 = 0x02, CONT03 = 0x03, CONT06 = 0x06,
  CONT07 = 0x07, CONT08 = 0x08, CONT09 = 0x09, CONT0A = 0x0A, CONT0D = 0x0D,
  CONT0E = 0x0E, CONT0F = 0x0F, CONT13 = 0x13, CONT14 = 0x14, CONT1A = 0x1A,
  CONT26 = 0x26, CONT2A = 0x2A
};

static uint8_t akAddr = 0;

static void led(int pin, bool on) { digitalWrite(pin, on ? LED_ON : !LED_ON); }

static void blinkN(int pin, int n) {
  for (int i = 0; i < n; i++) { led(pin, true); delay(200); led(pin, false); delay(200); }
  delay(600);
}

// Error state: amp stays off, red LED flashes fast forever
static void errorHalt() {
  digitalWrite(PIN_MUTE, MUTE_ON);
  digitalWrite(PIN_AMP_PWR, AMP_OFF);
  for (;;) { led(PIN_RED_LED, true); delay(80); led(PIN_RED_LED, false); delay(80); }
}

// Scan I2C. Returns number of devices; sets akAddr if an AK7755 address answered.
static int i2cScan() {
  int found = 0;
  akAddr = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      if (a == AK_ADDR_CAD_L || a == AK_ADDR_CAD_H) akAddr = a;
    }
  }
  return found;
}

// ---------------- AK7755 register access ----------------
// Write: S | addr+W | 0xC0|reg | data | P   [p.82]
static bool akWrite(uint8_t reg, uint8_t val) {
  for (int tries = 0; tries < 5; tries++) {
    Wire.beginTransmission(akAddr);
    Wire.write((uint8_t)(0xC0 | reg));
    Wire.write(val);
    if (Wire.endTransmission() == 0) return true;
    delay(1);   // RDY low -> no ACK, retry [p.83]
  }
  return false;
}

// Read: S | addr+W | 0x40|reg | Sr | addr+R | data | NACK | P   [p.82]
static int akRead(uint8_t reg) {
  Wire.beginTransmission(akAddr);
  Wire.write((uint8_t)(0x40 | reg));
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)akAddr, 1) != 1) return -1;
  return Wire.read();
}

// Step A: all settings that must be made while in clock reset / system reset.
// (After PDN release CKRESETN=0, CRESETN=0, DSPRESETN=0.)  [p.30, 52, 57]
static bool akConfigure() {
  bool ok = true;
  auto w = [&](uint8_t r, uint8_t v) { if (!akWrite(r, v)) ok = false; };

  // Clock generation (clock reset only), CKRESETN stays 0
  w(CONT00, (AK_CKM << 4) | AK_DFS);            // CKM mode, 48 kHz, AINE=0 (no analog in)
  w(CONT01, (AK_BITFS << 4));                   // BICK=64fs, CLKS=0, CKRESETN=0

  // Serial format: I2S, BICK falling edge (default)
  w(CONT02, (AK_LRIF << 4));
  w(CONT03, 0x00);                              // DIF2/DOF2 MSB 24-bit, delay RAM default
  w(CONT06, 0x00);                              // DAC input = MSB 24-bit (I2S), DIF1 MSB
  w(CONT07, 0x00);                              // DSP output formats default

  // Route SDINx straight to the DAC (bypass DSP, none loaded)
  w(CONT08, (AK_DAC_SRC << 6));                 // SELDAI, others default
  w(CONT09, 0x06);                              // LO3SW1|LO3SW2: OUT3 = DAC L+R mix
  w(CONT0A, AK_BICKLRCK_OE);                    // BICK/LRCK output only in master mode

  // Bits that datasheet says must be 1 during system reset  [p.42, 49, 51]
  w(CONT0D, 0x40);
  w(CONT1A, 0x30);                              // D4=1 (required) | DAMUTE=1 (start muted)
  w(CONT26, 0x01);
  w(CONT2A, 0x80);

  // Power management: DAC L/R + line-outs 1/2/3  [p.43]
  w(CONT0E, 0x1F);                              // PMLO3|PMLO2|PMLO1|PMDAR|PMDAL

  // Line-out volumes (LIGN=0 dB, LOVOL3 / LOVOL2 / LOVOL1)  [p.47-48]
  w(CONT13, AK_LINEOUT_VOL);
  w(CONT14, (AK_LINEOUT_VOL << 4) | AK_LINEOUT_VOL);
  // DAC digital volume CONT18/19 left at default 0 dB (18h)
  return ok;
}

// Step B: release clock reset -> PLL starts. BICK/LRCK (or XTI) must be running.
static bool akReleaseClockReset() {
  return akWrite(CONT01, (AK_BITFS << 4) | 0x01);   // CKRESETN = 1
}

// Step C: release CODEC + DSP reset -> DAC starts.
static bool akReleaseSystemReset() {
  return akWrite(CONT0F, 0x0C);                     // CRESETN | DSPRESETN
}

// Step D: read back key registers to confirm the chip really took the settings.
static bool akVerify() {
  int v00 = akRead(CONT00), v01 = akRead(CONT01), v0e = akRead(CONT0E),
      v0f = akRead(CONT0F), v0d = akRead(CONT0D);
  if (v00 != ((AK_CKM << 4) | AK_DFS)) return false;
  if (v01 != ((AK_BITFS << 4) | 0x01)) return false;
  if (v0e != 0x1F) return false;
  if (v0f != 0x0C) return false;
  if (v0d < 0 || !(v0d & 0x80)) return false;       // STO=1: normal operation, no internal error
  return true;
}

// ---------------- I2S ----------------
// 32-bit slots -> BCLK = 64 fs, matching BITFS=0. 16-bit samples are left-aligned.
static void i2sStart() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)((I2S_ROLE_MASTER ? I2S_MODE_MASTER : I2S_MODE_SLAVE) | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
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
  int32_t buf[N * 2];
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
      int32_t s = ((int32_t)(sinf(phase) * AMPLITUDE * env)) << 16;
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

  // 2. Let booster / 3V3 settle, then release AK7755 power-down.
  //    PDN must be L while supplies come up (it is), then H; registers are
  //    accessible >= 1 ms after PDN=H [p.52, 56].
  delay(RAIL_WAIT_MS);
  digitalWrite(PIN_DSP_PDN, !PDN_RUN); delay(20);
  digitalWrite(PIN_DSP_PDN, PDN_RUN);  delay(5);

  // 3. Scan I2C (AK7755 does not ACK while in power-down, so this must come after PDN release)
  Wire.begin(PIN_SDA, PIN_SCL, 400000);            // AK7755 supports up to 400 kHz [p.79]
  int n = i2cScan();
  blinkN(PIN_RED_LED, n);                          // 0 blinks = nothing found
  if (akAddr == 0) errorHalt();                    // AK7755 (0x18/0x19) did not answer

  // 4. Program registers while in clock reset
  if (!akConfigure()) errorHalt();

  // 5. Start I2S so BICK/LRCK are present before the PLL is started
  //    (in slave-I2S/master-DSP mode the AK7755 generates them after step 6)
  i2sStart();
  delay(20);

  // 6. Release clock reset, wait for PLL lock (10 ms min), then release CODEC/DSP reset
  if (!akReleaseClockReset()) errorHalt();
  delay(20);
  if (!akReleaseSystemReset()) errorHalt();
  delay(10);                                       // RAM clear takes ~400 us [p.58]
  if (!akVerify()) errorHalt();

  // 7. Amp supply on, settle, unmute (hardware mute pin, then DAC soft mute)
  digitalWrite(PIN_AMP_PWR, AMP_ON);
  delay(AMP_SETTLE_MS);
  digitalWrite(PIN_MUTE, MUTE_OFF);
  akWrite(CONT1A, 0x10);                           // DAMUTE=0 (keep D4=1)  [p.49,91]
  led(PIN_RED_LED, true);                          // solid red = reached tone phase
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
