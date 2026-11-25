#pragma once
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"

#include "audio_output.h"
#include "battery_monitor.h"
#include "bt_audio.h"
#include "onyx_ota_protocol.h"

class BtOta {
public:
  enum class LinkType : uint8_t {
    NONE        = 0,
    BLE         = 1,
    CLASSIC_SPP = 2,
  };

  // Call AFTER btAudio.begin() has started Bluedroid (esp_bluedroid_enable).
  bool begin(AudioOutput &audio, BatteryMonitor &battery, BtAudio &btAudio);

  // Confirm running OTA partition after healthy hardware + BT boot (cancels bootloader rollback).
  void confirmRunningFirmware();

  // Call frequently in loop(); drains OTA queue, flashes chunks on Core 1, sends telemetry.
  void poll();

  bool    isOtaActive()     const;
  uint8_t otaProgressPct()  const { return progressPct_; }
  bool    rebootRequested() const;

  // Read-only descriptor embedded in .rodata so .bin validation works on host & target.
  static const OnyxOta::FwDescriptor &firmwareDescriptor();

private:
  struct RxEvent {
    LinkType link;
    uint8_t  msgType;
    uint8_t  flags;
    uint16_t length;
    uint8_t  payload[OnyxOta::MAX_FRAME_PAYLOAD];
  };

  static constexpr size_t RX_QUEUE_DEPTH = 16;

  // Frame parser state for Classic SPP byte stream
  enum class ParseState : uint8_t {
    SYNC_0,
    SYNC_1,
    HEADER,
    PAYLOAD,
    CRC_LO,
    CRC_HI,
  };

  struct StreamParser {
    ParseState state   = ParseState::SYNC_0;
    uint8_t    msgType = 0;
    uint8_t    flags   = 0;
    uint16_t   length  = 0;
    uint16_t   pos     = 0;
    uint8_t    hdrIdx  = 0;
    uint8_t    hdrBuf[4] = {};
    uint8_t    payload[OnyxOta::MAX_FRAME_PAYLOAD] = {};
    uint16_t   rxCrc   = 0;

    void reset() {
      state  = ParseState::SYNC_0;
      pos    = 0;
      hdrIdx = 0;
    }
  };

  bool enqueueEvent(LinkType link, uint8_t msgType, uint8_t flags, const uint8_t *payload, uint16_t len);
  bool dequeueEvent(RxEvent &out);

  void feedStreamBytes(LinkType link, StreamParser &parser, const uint8_t *data, size_t len);
  bool parseSingleFrame(LinkType link, const uint8_t *data, size_t len);

  void handleEvent(const RxEvent &ev);
  void handleHello(LinkType link);
  void handleOtaBegin(LinkType link, const uint8_t *payload, uint16_t len);
  void handleOtaData(LinkType link, const uint8_t *payload, uint16_t len);
  void handleOtaCommit(LinkType link);
  void handleOtaReboot(LinkType link);
  void handleOtaAbort(LinkType link, bool sendReply);

  void fillDeviceInfo(OnyxOta::DeviceInfoPayload &info, LinkType forLink) const;
  bool verifyWrittenDescriptor(const esp_partition_t *part, uint32_t imageSize) const;
  void abortActiveOtaInternal();

  bool sendFrame(LinkType link, uint8_t msgType, const void *payload, uint16_t len);
  void sendAck(LinkType link, uint8_t origCmd, uint8_t statusCode,
               uint16_t chunkSize, uint32_t nextOffset, uint32_t detail = 0);
  void sendProgress(LinkType link);
  void sendError(LinkType link, uint8_t origCmd, uint8_t errCode,
                 uint32_t expectedOffset, const char *msg);

  bool setupClassicEir();
  bool setupClassicSpp();
  bool setupBleGatt();

  static BtOta *instance_;

  AudioOutput    *audio_   = nullptr;
  BatteryMonitor *battery_ = nullptr;
  BtAudio        *btAudio_ = nullptr;

  // Classic SPP state
  volatile bool     sppReady_     = false;
  volatile bool     sppConnected_ = false;
  volatile bool     sppCongested_ = false;
  volatile uint32_t sppHandle_    = 0;
  StreamParser      sppParser_;

  // BLE GATT state
  volatile bool     bleReady_          = false;
  volatile bool     bleConnected_      = false;
  volatile uint16_t bleConnId_         = 0;
  volatile uint8_t  bleGattsIf_        = 0xFF;
  volatile uint16_t bleMtu_            = 23;
  volatile bool     bleInfoNotify_     = false;
  volatile bool     bleCtrlNotify_     = false;
  volatile uint8_t  bleAdvCfgMask_     = 0;
  uint16_t          gattHandles_[9]    = {};

  // Active OTA session state
  OnyxOta::OtaState       otaState_          = OnyxOta::STATE_IDLE;
  LinkType                activeOtaLink_     = LinkType::NONE;
  const esp_partition_t  *updatePartition_   = nullptr;
  esp_ota_handle_t        otaHandle_         = 0;
  bool                    otaHandleOpen_     = false;
  uint32_t                expectedSize_      = 0;
  uint32_t                bytesReceived_     = 0;
  uint32_t                lastAckedOffset_   = 0;
  uint32_t                windowBytes_       = 4096;
  uint16_t                activeChunkSize_   = 512;
  uint8_t                 activeWindowPkts_  = 8;
  uint8_t                 expectedSha256_[32] = {};
  mbedtls_sha256_context  shaCtx_;
  bool                    shaActive_         = false;
  uint8_t                 progressPct_       = 0;
  uint32_t                lastMismatchErrMs_ = 0;
  uint32_t                lastActivityMs_    = 0;
  uint32_t                lastTelemetryMs_   = 0;
  bool                    rollbackConfirmed_ = false;
  bool                    rebootDue_         = false;
  uint32_t                rebootAtMs_        = 0;

  // Lock-protected RX ring buffer between Bluedroid callbacks (Core 0) and poll() (Core 1)
  portMUX_TYPE  rxMux_  = portMUX_INITIALIZER_UNLOCKED;
  uint8_t       rxHead_ = 0;
  uint8_t       rxTail_ = 0;
  RxEvent       rxQueue_[RX_QUEUE_DEPTH] = {};

  friend struct BtOtaCallbacks;
};
