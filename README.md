# Harman Kardon Onyx Studio 5/6 — ESP32 Replacement Firmware

Custom firmware replacing the factory **BM20IK / BES2000** Bluetooth module inside a **Harman Kardon Onyx Studio 5/6** portable speaker with a classic **ESP32 DevKit**, while retaining the original mainboard audio, power, and charging hardware:

- **AK7755** DSP/DAC (configured in zero-DSP DAC bypass + stereo Aux-In ADC mode)
- **Dual TPA3116D2** Class-D amplifiers (`U19` 2 Ω woofer, `U20` 4 Ω tweeter)
- **SLG4V4826** GreenPAK standby / power latch sequencer
- **SY7901** 10 V battery boost, **SY8213** 3.3 V / 4.7 V bucks, **bq24090** 1S Li-ion charger
- **Dual-Mode Bluetooth OTA** (Classic SPP + BLE GATT + rollback protection) for updating the sealed speaker wirelessly

Current firmware version: **`v1.0.7`** (`[env:esp32dev]`).

---

## 1. Dual-Core Firmware Architecture

```
Core 0 (PRO_CPU — Bluedroid BT Stack)          Core 1 (APP_CPU — Real-Time Audio + Control)
┌────────────────────────────────────┐         ┌──────────────────────────────────────────────────┐
│ • Classic A2DP Sink (44.1 / 48 kHz)│  SPSC   │ dsp_core1 Task (Priority 5, 6 KB stack)          │
│ • AVRCP Controller & Target        ├────────►│  1. Mid/Side Spatial Diffuser (Harman CSF feel)  │
│ • Dual-Pair & Auto-Reconnect (NVS) │ 4096-fr │  2. Signal+Vol ISO 226 Dynamic Loudness EQ       │
│ • Dual-Mode OTA RX (BLE + SPP)     │  ring   │  3. 5-Band Studio 9 EQ + 92 Hz Dynamic Bass DRC  │
│                                    │         │  4. 2.5 kHz LR4 Crossover + 150 µs Tweeter Delay │
└────────────────────────────────────┘         │  5. 64-bit Sliding Subsonic HPF (40..70 Hz)      │
                                               │  6. Look-Ahead Peak + RMS Limiters -> I2S0 TX    │
                                               │                                                  │
                                               │ Arduino loop() (Priority 1)                      │
                                               │  • Real Power Button (SLG D17) clean shutdown    │
                                               │  • OTA flash writer + SHA-256 + rollback confirm │
                                               │  • Battery/AC monitor, Aux detect, Buttons, LEDs │
                                               └──────────────────────────────────────────────────┘
```

### Source Module Map
| Module | Files | Responsibility |
|---|---|---|
| Hardware Config | [`include/board_config.h`](include/board_config.h) | Pin map, verified polarities, timings, crossover, DRC, loudness & battery thresholds |
| Power Latch & Off | [`include/power_manager.h`](include/power_manager.h), [`src/power_manager.cpp`](src/power_manager.cpp) | SLG4V4826 latch (`D4`), real Power Button off (`D17`), USB wake-restart |
| AK7755 Codec | [`include/ak7755.h`](include/ak7755.h), [`src/ak7755.cpp`](src/ak7755.cpp) | 100 kHz I2C bring-up (`0x18`), DAC bypass (`SDIN1→OUT1/OUT2`), Aux ADC (`IN1/IN3→SDOUT3`) |
| Core 1 DSP Engine | [`include/dsp_crossover.h`](include/dsp_crossover.h), [`src/dsp_crossover.cpp`](src/dsp_crossover.cpp), [`include/eq_coeffs.h`](include/eq_coeffs.h) | Spatializer, dynamic loudness, Studio 9 EQ + dynamic bass DRC, 2.5 kHz LR4 crossover, Biquad64 subsonic HPF, limiters |
| Audio I/O & SysID | [`include/audio_output.h`](include/audio_output.h), [`src/audio_output.cpp`](src/audio_output.cpp) | Full-duplex I2S0 (`APLL`), Core 1 DSP task, triple-safe Aux-In, ADPCM chimes, glitch-free 48 kHz Farina ESS generator |
| Bluetooth Audio | [`include/bt_audio.h`](include/bt_audio.h), [`src/bt_audio.cpp`](src/bt_audio.cpp) | A2DP Sink, AVRCP CT/TG, Dual-Pair, NVS 2-device auto-reconnect |
| Dual-Mode BT OTA | [`include/bt_ota.h`](include/bt_ota.h), [`src/bt_ota.cpp`](src/bt_ota.cpp), [`include/onyx_ota_protocol.h`](include/onyx_ota_protocol.h) | BLE GATT + Classic SPP OTA flashing, SHA-256 verification, descriptor check, rollback confirmation |
| Front Controls | [`include/buttons.h`](include/buttons.h), [`src/buttons.cpp`](src/buttons.cpp) | Auto-learned idle polarity debounce, multi-tap (`BLUE` / `PLAY`), SysID trigger |
| Power / Battery | [`include/battery_monitor.h`](include/battery_monitor.h), [`src/battery_monitor.cpp`](src/battery_monitor.cpp) | `BAT_DET` (VN), `BAT_TEMP_DET` (VP), `DC_DET` (19 V AC vs 10 V Bat DSP rail switching), `/CHG` |
| PWM Status LEDs | [`include/status_led.h`](include/status_led.h), [`src/status_led.cpp`](src/status_led.cpp) | 5 kHz gamma-corrected hardware PWM fading on WHITE (`TX0`) and RED (`D12`) |
| Harman Chimes | [`include/chime_data.h`](include/chime_data.h), [`src/chime_data.cpp`](src/chime_data.cpp) | IMA-ADPCM chimes (power on/off/pairing) upsampled via Catmull-Rom |

---

## 2. Verified Hardware Pinout & Critical Polarities

> **Warning — No USB Serial in Production:** `TX0` (GPIO1) drives `WHITE_LED` and `RX0` (GPIO3) drives `AMP_PWR_CTRL`. Never call `Serial.begin()` in production firmware.

| ESP32 GPIO | Board Net | Direction | Verified Polarity / Hardware Notes |
|---|---|---|---|
| **GPIO 21 / 22** | `I2C_SDA` / `I2C_SCL` | I/O | 100 kHz I2C to AK7755 (`0x18`, `CAD` tied GND on DSP side of R87) |
| **GPIO 25 / 14 / 26** | `I2S_BCLK` / `LRCLK` / `DOUT` | Out | Master I2S0 (32-bit, APLL) → AK7755 `SDIN1` (`OUT1`=Woofer, `OUT2`=Tweeter) |
| **GPIO 27** | `I2S_DIN` | **Input** | Driven by AK7755 `SDOUT3` (pin 14, Aux-In stereo ADC from `IN1` pin 34 = R, `IN3` pin 32 = L) |
| **GPIO 5** | `DSP_PDN` | Out | `HIGH` = run, `LOW` = reset. Wait `50 ms` after rising edge (`C92=10 µF`, `C90=2.2 µF`) |
| **GPIO 3 (RX0)** | `AMP_PWR_CTRL` | Out | `HIGH` = amp rails ON (Q2 NPN → Q18 ME4435 P-FET), `LOW` = OFF |
| **GPIO 19** | `MUTE` | Out | **CRITICAL:** `LOW` = **MUTED**, `HIGH` = **UNMUTED** (Q17 NPN inverts TPA3116 active-HIGH pin 12) |
| **GPIO 4** | `Power_Off_Ready` | Out | To SLG4V4826 pin 5: must assert `HIGH` within ~900 ms of wake to latch power; `LOW` cuts power |
| **GPIO 17** | `Power_On_Off` | **Input** | From SLG4V4826 pin 4 (via R238 100 Ω): pulses `HIGH` (~270 ms) when real Power Button is pressed |
| **GPIO 15** | `VREG_EN` | Input | From SLG4V4826 pin 11 (`TP34`), `HIGH` while latched ON |
| **GPIO 1 (TX0) / 12** | `WHITE_LED` / `RED_LED` | Out | Active-HIGH PWM LEDs (`AW9110B` / Blue LED not populated on this board) |
| **GPIO 32 / 18** | `BTN_BLUE` / `BTN_PLAY_PAUSE` | Input | Working front buttons (auto-learned idle polarity) |
| **GPIO 33 / 13** | `BTN_VOL_UP` / `BTN_VOL_DOWN` | Input | *Non-functional on this hardware unit — do not rely on for core controls* |
| **GPIO 39 (VN)** | `BAT_DET` | ADC | 300k / 100k divider → `V_bat = 4.0 × V_adc` |
| **GPIO 36 (VP)** | `BAT_TEMP_DET` | ADC | 10k NTC ∥ 4.7k divider |
| **GPIO 34 / 23 / 35** | `DC_DET` / `CHG_STATUS` / `AUX_IN_DET` | Input | `DC_DET` HIGH on 19 V AC; `CHG_STATUS` active-LOW; `AUX_IN_DET` jack detect |

### Hardware Rules & Safeguards
- **Manual power-off only:** All automatic idle / low-battery / thermal auto-shutdown triggers are intentionally disabled in [`src/main.cpp`](src/main.cpp) — only the physical Power Button shuts down the speaker.
- **Off-State Charging Mode (`v1.0.7`):** Plugging in the `19 V` charger powers `U13`/`bq24090` (`~0.8 A` hardware battery charge) and also enables `U14` (`SYS_3V3`). On cold charger plug-in (`VREG_EN == LOW && DC_DET == HIGH`) or after turning OFF while plugged in, the ESP32 keeps amps, AK7755, and Bluetooth **OFF**, smoothly **breathes the RED LED** while charging, turns it **OFF** when 100% full, and wakes immediately when the physical Power Button asserts `VREG_EN` HIGH. Software/OTA reboots (`ESP_RST_SW`) always boot directly into active mode to confirm OTA rollback cleanly.
- **Sealed-box OTA safety:** Never OTA-flash `[env:sysid_capture]` (it excludes `bt_ota.cpp`). Always build and OTA-flash `[env:esp32dev]`, which includes Dual-Mode BT OTA, [`partitions_ota.csv`](partitions_ota.csv), and `btOta.confirmRunningFirmware()`.

---

## 3. Button & LED Controls

### Normal Mode (Bluetooth + Aux-In)
| Action | Function |
|---|---|
| **Power Button** | Clean pop-free shutdown (chime → soft-mute → amp off → SLG latch release) |
| **`PLAY/PAUSE` 1× / 2× / 3×** | Play/Pause / Next Track / Previous Track (AVRCP) |
| **`PLAY/PAUSE` hold (1.5 s)** | Manually toggle Aux-In (AK7755 pins 34/32) ↔ Bluetooth *(also auto-switches on Aux signal/jack)* |
| **`BLUE` 1×** | Open 60 s Dual-Pair window (fast WHITE fade) |
| **`BLUE` 2×** | Swap between the 2 NVS-saved paired phones |
| **`BLUE` hold (2.5 s)** | Disconnect current device & enter fresh pairing mode |
| **`BLUE` 3×** *(or `BLUE + PLAY` hold 1.5 s)* | Enter / exit **48 kHz SysID Measurement Mode** (OTA-safe) |

### 48 kHz SysID Measurement Mode
| Action | Function |
|---|---|
| **`BLUE` 1×** or **`PLAY/PAUSE` 1×** | 3-second RED blink countdown → exact `-20 dBFS` 48 kHz Farina ESS (`ess_single_m20db` match) |
| **`BLUE` 2×** | Toggle `FLAT_BASELINE` (**solid WHITE**) ↔ `TUNED` (**breathing WHITE**) |
| **`BLUE` 3×** *(or `BLUE + PLAY` hold 1.5 s)* | Exit back to normal Bluetooth/Aux mode |

### LED Guide
- **WHITE slow breathing:** Pairing mode (or `TUNED` mode when inside SysID mode)
- **WHITE fast breathing:** 60 s Dual-Pair window open
- **WHITE solid (100%):** Bluetooth connected (or `FLAT_BASELINE` when inside SysID mode)
- **WHITE soft steady (35%):** Aux-In active
- **RED slow breathing:** Charging on 19 V adapter (`bq24090 /CHG` active)
- **RED solid:** Low battery warning (`≤ 3.35 V` on battery)
- **RED repeating N blinks:** Fatal boot error (`2–5` = AK7755 I2C/init, `6` = Bluetooth init)

---

## 4. Building & Flashing

Always use PlatformIO Core 6.2+ from the PlatformIO venv (`~/.platformio/penv/bin/pio`):

```bash
# Build OTA-safe production firmware (outputs .pio/build/esp32dev/firmware.bin)
/home/joseph/.platformio/penv/bin/pio run -e esp32dev
```

- **OTA Partition Table:** [`partitions_ota.csv`](partitions_ota.csv) — dual `0x1F0000` (~1.94 MiB) OTA slots (`app0` / `app1`).
- **Companion OTA Uploader:** [`qt_ota_app/`](qt_ota_app/) flashes `.pio/build/esp32dev/firmware.bin` over Bluetooth and verifies `ONYX_FW_MAJOR/MINOR/PATCH` (`1.0.6`), `ONYX_PRODUCT_ID` (`0x0601`), and SHA-256.
- **Acoustic SysID & Tuning Toolkit:** See [`docs/DSP_AND_TUNING_LOG.md`](docs/DSP_AND_TUNING_LOG.md) and [`tools/README.md`](tools/README.md).
