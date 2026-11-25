#include "ak7755.h"
#include "board_config.h"
#include <Wire.h>

using namespace BoardConfig;

namespace {
constexpr uint8_t CONT00_AINE   = 0x08; // Enable analog inputs IN1/IN3 (Aux-In)
constexpr uint8_t CONT08_VAL    = (3 << 6) | (1 << 4); // SELDAI=3 (SDIN1->DAC), SELDO3=1 (MIXOUT->SDOUT3)
constexpr uint8_t CONT0A_VAL    = 0x04; // OUT3E=1 (SDOUT3 output to ESP32 I2S_DIN D27)
constexpr uint8_t CONT0E_VAL    = 0xCF; // PMADR|PMADL (Aux ADC) | PMLO2|PMLO1|PMDAR|PMDAL (DAC+OUT1/2)
} // namespace

uint8_t Ak7755::dfsForRate(uint32_t sampleRate) {
  return (sampleRate == 32000) ? 4 : 5; // DFS=4 (32 kHz), DFS=5 (44.1 / 48 kHz)
}

void Ak7755::beginPins() {
  pinMode(PIN_DSP_PDN, OUTPUT);
  holdPowerDown();
}

void Ak7755::holdPowerDown() {
  digitalWrite(PIN_DSP_PDN, !PDN_RUN);
}

void Ak7755::releasePowerDown() {
  digitalWrite(PIN_DSP_PDN, !PDN_RUN);
  delay(PDN_RESET_MS);
  digitalWrite(PIN_DSP_PDN, PDN_RUN);
  delay(PDN_SETTLE_MS);
}

int Ak7755::scanBus() {
  int found = 0;
  addr_ = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      if (a == ADDR_CAD_L || a == ADDR_CAD_H) addr_ = a;
    }
  }
  return found;
}

bool Ak7755::writeReg(uint8_t reg, uint8_t val) {
  for (int tries = 0; tries < 5; tries++) {
    Wire.beginTransmission(addr_);
    Wire.write(static_cast<uint8_t>(0xC0 | reg));
    Wire.write(val);
    if (Wire.endTransmission() == 0) return true;
    delay(1); // RDY low -> retry [datasheet p.83]
  }
  return false;
}

int Ak7755::readReg(uint8_t reg) {
  Wire.beginTransmission(addr_);
  Wire.write(static_cast<uint8_t>(0x40 | reg));
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(static_cast<int>(addr_), 1) != 1) return -1;
  return Wire.read();
}

bool Ak7755::configureBypass(uint8_t wooferVol, uint8_t tweeterVol, uint32_t sampleRate) {
  dfs_ = dfsForRate(sampleRate);
  bool ok = true;
  auto w = [&](uint8_t r, uint8_t v) { if (!writeReg(r, v)) ok = false; };

  // Datasheet p.31: set AINE=1 FIRST so IN1/IN3 (Aux-In) input coupling caps pre-charge
  w(CONT00, (CKM_SLAVE_BICK << 4) | CONT00_AINE | dfs_);
  w(CONT01, (BITFS_64FS << 4));
  w(CONT02, (LRIF_I2S << 4));
  w(CONT03, 0x00);
  w(CONT06, 0x00);                               // DAC input MSB 24-bit (I2S)
  w(CONT07, 0x00);

  // SDIN1 -> DAC directly (DSP bypass); MIXOUT (stereo ADC IN1/IN3) -> SDOUT3 (ESP32 D27)
  w(CONT08, CONT08_VAL);
  w(CONT09, 0x00);                               // IN1/IN3 single-ended ADC, SELMIX=000, OUT3 off
  w(CONT0A, CONT0A_VAL);                         // OUT3E=1 (SDOUT3 enabled), BICK/LRCK inputs

  // Required "1" bits during system reset [datasheet p.42, 49, 51]
  w(CONT0D, 0x40);
  w(CONT1A, 0x30);                               // D4=1 (required) | DAMUTE=1
  w(CONT26, 0x01);
  w(CONT2A, 0x80);

  // Power up stereo ADC (Aux-In pins 34/32: IN1/IN3) + DAC L/R + OUT1 (woofer) + OUT2 (tweeter)
  w(CONT0E, CONT0E_VAL);
  w(CONT12, static_cast<uint8_t>((AK_AUX_ADC_GAIN << 4) | (AK_AUX_ADC_GAIN & 0x0F))); // IN1/IN3 gain
  w(CONT13, 0x00);                               // OUT3 mute
  w(CONT14, static_cast<uint8_t>((tweeterVol << 4) | (wooferVol & 0x0F)));
  w(CONT15, 0x30);                               // ADC Lch (IN1 / pin 34) digital volume = 0.0 dB
  w(CONT16, 0x30);                               // ADC Rch (IN3 / pin 32) digital volume = 0.0 dB
  w(CONT1E, 0x00);                               // Analog inputs on pins 31-34 (not DMIC)
  return ok;
}

bool Ak7755::releaseClockReset() {
  return writeReg(CONT01, (BITFS_64FS << 4) | 0x01); // CKRESETN=1
}

bool Ak7755::releaseCodecReset() {
  return writeReg(CONT0F, 0x08);                     // CRESETN=1 only (DSP remains in reset)
}

bool Ak7755::verify() {
  int v00 = readReg(CONT00);
  int v01 = readReg(CONT01);
  int v08 = readReg(CONT08);
  int v0a = readReg(CONT0A);
  int v0e = readReg(CONT0E);
  int v0f = readReg(CONT0F);
  int v0d = readReg(CONT0D);

  if (v00 != ((CKM_SLAVE_BICK << 4) | CONT00_AINE | dfs_)) return false;
  if (v01 != ((BITFS_64FS << 4) | 0x01))                   return false;
  if (v08 != CONT08_VAL)                                   return false;
  if (v0a != CONT0A_VAL)                                   return false;
  if (v0e != CONT0E_VAL)                                   return false;
  if (v0f != 0x08)                                         return false;
  if (v0d < 0 || !(v0d & 0x80))                            return false; // STO=1
  return true;
}

bool Ak7755::setSoftMute(bool muted) {
  return writeReg(CONT1A, muted ? 0x30 : 0x10);      // Keep D4=1
}

bool Ak7755::enterClockResetForRate(uint32_t sampleRate) {
  dfs_ = dfsForRate(sampleRate);
  bool ok = true;
  ok &= writeReg(CONT0F, 0x00);                      // System reset
  ok &= writeReg(CONT01, (BITFS_64FS << 4));         // CKRESETN=0
  ok &= writeReg(CONT00, (CKM_SLAVE_BICK << 4) | CONT00_AINE | dfs_);
  return ok;
}
