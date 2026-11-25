#pragma once
#include <stdint.h>

enum class ButtonEvent : uint8_t {
  NONE = 0,
  VOL_UP,
  VOL_DOWN,
  PLAY_PAUSE,    // PLAY single press
  NEXT_TRACK,    // PLAY double press
  PREV_TRACK,    // PLAY triple press
  PLAY_LONG,     // PLAY hold (1.5 s): manually toggle Aux-In <-> Bluetooth
  BLUE_SHORT,    // BLUE single press: open dual-pair / pairing window (or run sweep in SysID mode)
  BLUE_DOUBLE,   // BLUE double press: swap paired phones (or toggle FLAT/TUNED in SysID mode)
  BLUE_TRIPLE,   // BLUE triple press: enter/exit 48 kHz SysID capture mode (OTA-safe!)
  BLUE_LONG,     // BLUE hold (2.5 s): disconnect & enter fresh pairing mode
  SYSID_TOGGLE   // BLUE + PLAY held together (1.5 s): also enters/exits 48 kHz SysID capture mode
};

class Buttons {
public:
  void begin();
  ButtonEvent poll();

private:
  struct Key {
    int      pin          = -1;
    int      idleLevel    = 1;
    bool     debounced    = false;
    bool     lastRaw      = false;
    uint32_t rawChangedMs = 0;
    uint32_t pressedAtMs  = 0;
    uint32_t lastRepeatMs = 0;
    uint32_t lastRelMs    = 0;
    uint8_t  clickCount   = 0;
    bool     longFired    = false;
  };

  bool updateDebounce(Key &k, uint32_t now, bool &rose, bool &fell);

  Key      volUp_;
  Key      volDown_;
  Key      play_;
  Key      blue_;
  uint32_t volComboStartMs_ = 0;
  bool     volComboFired_   = false;
};
