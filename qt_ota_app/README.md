# Onyx Studio Companion — Qt 6 Dual-Mode Bluetooth OTA App (Desktop & Android)

Cross-platform Qt 6 Quick/QML + C++20 companion app for discovering and updating **HK Onyx Studio** speakers running the dual-mode Bluetooth OTA firmware (`BLE GATT` + `Bluetooth Classic SPP`).

## Exclusive Brand Discovery (Bose / JBL Style)
Only speakers running this firmware appear in the app. All unrelated Bluetooth devices are hidden via three layers:
1. **BLE Advertisement Filter**: Matches custom 128-bit Service UUID `4f4e5958-5336-4f54-4100-000000000001` or Manufacturer Specific Data (`Company ID 0x594F`, magic `"OS"`).
2. **Bluetooth Classic Filter**: Matches Classic EIR Manufacturer Data (`0x594F` + `"OS"`) and probes connected/paired Classic devices via SDP for service name `ONYX_S6_OTA`.
3. **Protocol Handshake + Binary Signature**: On connect, `CMD_HELLO` must return `magic == "ONYX"`. Selected `.bin` files must contain ESP32 magic `0xE9` and the embedded `ONYX_FW_DESC_V1` descriptor matching the speaker's `productId`.

## Building for Desktop (Linux / Windows / macOS)
Requires Qt 6.5+ with `Qt6::Quick`, `Qt6::QuickControls2`, and `Qt6::Bluetooth`:

```bash
cmake -S qt_ota_app -B qt_ota_app/build -G Ninja
cmake --build qt_ota_app/build
./qt_ota_app/build/onyx_ota_app
```

## Building for Android 12+ (API 31+, APK / AAB)
1. Open `qt_ota_app/CMakeLists.txt` in **Qt Creator** with a **Qt 6 for Android (`arm64-v8a`)** kit, **or** build from the CLI using your Qt Android toolchain:
   ```bash
   JAVA_HOME=/usr/lib/jvm/java-25-openjdk \
   ANDROID_SDK_ROOT=$HOME/Android/Sdk \
   ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018 \
   $HOME/Qt/6.11.1/android_arm64_v8a/bin/qt-cmake \
       -S qt_ota_app -B qt_ota_app/build-android-arm64 -G Ninja \
       -DQT_HOST_PATH=$HOME/Qt/6.11.1/gcc_64 \
       -DANDROID_SDK_ROOT=$HOME/Android/Sdk \
       -DANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018 \
       -DCMAKE_BUILD_TYPE=Release
   JAVA_HOME=/usr/lib/jvm/java-25-openjdk \
   cmake --build qt_ota_app/build-android-arm64 --target apk
   ```
2. `android/AndroidManifest.xml` targets **Android 12+ (`minSdkVersion = 31`, `targetSdkVersion = 35`)** with `BLUETOOTH_SCAN` (`neverForLocation`) and `BLUETOOTH_CONNECT`, and the app requests `QBluetoothPermission` at runtime.

## First-Time Speaker Setup
Because the firmware switches from `huge_app.csv` (single 3 MB slot) to `partitions_ota.csv` (dual ~1.94 MB A/B OTA slots), flash the first OTA-capable build **once over USB**:

```bash
pio run -e esp32dev -t upload
```

For all subsequent updates:
1. Bump `ONYX_FW_MAJOR` / `ONYX_FW_MINOR` / `ONYX_FW_PATCH` in `platformio.ini`.
2. Build with `pio run -e esp32dev`.
3. Select `.pio/build/esp32dev/firmware.bin` in **Onyx Studio OTA** and tap **Install Firmware Update**.
