#include "buttons.h"
#include "board_config.h"

using namespace BoardConfig;

namespace {
constexpr uint32_t DEBOUNCE_MS      = 40;
constexpr uint32_t MULTI_CLICK_MS   = 320;
constexpr uint32_t VOL_HOLD_DELAY   = 450;
constexpr uint32_t VOL_REPEAT_MS    = 180;
constexpr uint32_t PLAY_LONG_MS     = 1500;
constexpr uint32_t VOL_COMBO_MS     = 1500;
constexpr uint32_t BLUE_LONG_MS     = 2500;
} // namespace

void Buttons::begin() {
  auto initKey = [](Key &k, int pin) {
    k.pin = pin;
    pinMode(pin, INPUT_PULLUP);
    delay(2);
    k.idleLevel = digitalRead(pin); // Auto-learn polarity at boot (handles active-LOW or active-HIGH)
  };

  initKey(volUp_,   PIN_BTN_VOL_UP);
  initKey(volDown_, PIN_BTN_VOL_DOWN);
  initKey(play_,    PIN_BTN_PLAY_PAUSE);
  initKey(blue_,    PIN_BTN_BLUE);
}

bool Buttons::updateDebounce(Key &k, uint32_t now, bool &rose, bool &fell) {
  rose = false;
  fell = false;
  bool raw = (digitalRead(k.pin) != k.idleLevel);
  if (raw != k.lastRaw) {
    k.lastRaw      = raw;
    k.rawChangedMs = now;
  }
  if ((now - k.rawChangedMs) >= DEBOUNCE_MS && raw != k.debounced) {
    k.debounced = raw;
    if (raw) {
      rose = true;
      k.pressedAtMs  = now;
      k.lastRepeatMs = now;
      k.longFired    = false;
    } else {
      fell = true;
      k.lastRelMs = now;
    }
  }
  return k.debounced;
}

ButtonEvent Buttons::poll() {
  uint32_t now = millis();
  bool roseUp = false, fellUp = false;
  bool roseDn = false, fellDn = false;
  bool upHeld = updateDebounce(volUp_,   now, roseUp, fellUp);
  bool dnHeld = updateDebounce(volDown_, now, roseDn, fellDn);

  // --- VOL+ (tap + hold repeat) ---
  if (upHeld) {
    if (roseUp) return ButtonEvent::VOL_UP;
    if ((now - volUp_.pressedAtMs) >= VOL_HOLD_DELAY &&
        (now - volUp_.lastRepeatMs) >= VOL_REPEAT_MS) {
      volUp_.lastRepeatMs = now;
      return ButtonEvent::VOL_UP;
    }
  }

  // --- VOL- (tap + hold repeat) ---
  if (dnHeld) {
    if (roseDn) return ButtonEvent::VOL_DOWN;
    if ((now - volDown_.pressedAtMs) >= VOL_HOLD_DELAY &&
        (now - volDown_.lastRepeatMs) >= VOL_REPEAT_MS) {
      volDown_.lastRepeatMs = now;
      return ButtonEvent::VOL_DOWN;
    }
  }

  // --- PLAY/PAUSE + BLUE combo (hold 1.5 s together -> toggle 48 kHz SysID capture mode) ---
  bool rosePlay = false, fellPlay = false;
  bool roseBlue = false, fellBlue = false;
  bool playHeld = updateDebounce(play_, now, rosePlay, fellPlay);
  bool blueHeld = updateDebounce(blue_, now, roseBlue, fellBlue);

  if (playHeld && blueHeld) {
    if (volComboStartMs_ == 0) {
      volComboStartMs_ = now;
      volComboFired_   = false;
    } else if (!volComboFired_ && (now - volComboStartMs_) >= VOL_COMBO_MS) {
      volComboFired_    = true;
      play_.longFired   = true;
      play_.clickCount  = 0;
      blue_.longFired   = true;
      blue_.clickCount  = 0;
      return ButtonEvent::SYSID_TOGGLE;
    }
    return ButtonEvent::NONE;
  } else if (!playHeld && !blueHeld) {
    volComboStartMs_ = 0;
    volComboFired_   = false;
  }

  // --- PLAY/PAUSE (1x play/pause, 2x next, 3x prev, hold 1.5s toggle Aux-In) ---
  if (playHeld && !volComboFired_) {
    if (!play_.longFired && (now - play_.pressedAtMs) >= PLAY_LONG_MS) {
      play_.longFired  = true;
      play_.clickCount = 0;
      return ButtonEvent::PLAY_LONG;
    }
  }
  if (fellPlay && !play_.longFired) {
    play_.clickCount++;
  }
  if (!play_.debounced && play_.clickCount > 0 && (now - play_.lastRelMs) >= MULTI_CLICK_MS) {
    uint8_t n = play_.clickCount;
    play_.clickCount = 0;
    if (n == 1) return ButtonEvent::PLAY_PAUSE;
    if (n == 2) return ButtonEvent::NEXT_TRACK;
    return ButtonEvent::PREV_TRACK;
  }

  // --- BLUE (1x dual-pair, 2x swap phone, 3x SysID capture toggle, hold 2.5s disconnect & pair) ---
  if (blueHeld && !volComboFired_) {
    if (!blue_.longFired && (now - blue_.pressedAtMs) >= BLUE_LONG_MS) {
      blue_.longFired  = true;
      blue_.clickCount = 0;
      return ButtonEvent::BLUE_LONG;
    }
  }
  if (fellBlue && !blue_.longFired) {
    blue_.clickCount++;
  }
  if (!blue_.debounced && blue_.clickCount > 0 && (now - blue_.lastRelMs) >= MULTI_CLICK_MS) {
    uint8_t n = blue_.clickCount;
    blue_.clickCount = 0;
    if (n == 1) return ButtonEvent::BLUE_SHORT;
    if (n == 2) return ButtonEvent::BLUE_DOUBLE;
    return ButtonEvent::BLUE_TRIPLE;
  }

  return ButtonEvent::NONE;
}
