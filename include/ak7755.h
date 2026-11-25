#pragma once
#include <stdint.h>

class Ak7755 {
public:
  static constexpr uint8_t ADDR_CAD_L = 0x18;
  static constexpr uint8_t ADDR_CAD_H = 0x19;

  void beginPins();
  void holdPowerDown();
  void releasePowerDown();

  // Scans I2C bus; returns total devices found and records AK7755 address if present
  int  scanBus();
  bool isFound() const { return addr_ != 0; }
  uint8_t address() const { return addr_; }

  bool configureBypass(uint8_t wooferVol, uint8_t tweeterVol, uint32_t sampleRate);
  bool releaseClockReset();
  bool releaseCodecReset();
  bool verify();

  bool setSoftMute(bool muted);
  bool enterClockResetForRate(uint32_t sampleRate);

private:
  enum Reg : uint8_t {
    CONT00 = 0x00, CONT01 = 0x01, CONT02 = 0x02, CONT03 = 0x03, CONT06 = 0x06,
    CONT07 = 0x07, CONT08 = 0x08, CONT09 = 0x09, CONT0A = 0x0A, CONT0D = 0x0D,
    CONT0E = 0x0E, CONT0F = 0x0F, CONT12 = 0x12, CONT13 = 0x13, CONT14 = 0x14,
    CONT15 = 0x15, CONT16 = 0x16, CONT1A = 0x1A, CONT1E = 0x1E, CONT26 = 0x26,
    CONT2A = 0x2A
  };

  static constexpr uint8_t CKM_SLAVE_BICK = 3; // PLL locks to BICK
  static constexpr uint8_t BITFS_64FS     = 0; // 32-bit stereo slots
  static constexpr uint8_t LRIF_I2S       = 1; // Standard I2S
  static constexpr uint8_t SELDAI_SDIN1   = 3; // SDIN1 -> DAC directly (DSP bypass)

  static uint8_t dfsForRate(uint32_t sampleRate);
  bool writeReg(uint8_t reg, uint8_t val);
  int  readReg(uint8_t reg);

  uint8_t addr_ = 0;
  uint8_t dfs_  = 5;
};
