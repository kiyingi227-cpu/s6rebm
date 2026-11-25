#pragma once
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Onyx Studio Dual-Mode Bluetooth (Classic SPP + BLE GATT) OTA Protocol
// Shared specification between ESP32 firmware and Qt 6 Companion App.
// ============================================================================

namespace OnyxOta {

// ---------------- Firmware Version Defaults ----------------
#ifndef ONYX_FW_MAJOR
#define ONYX_FW_MAJOR 1
#endif
#ifndef ONYX_FW_MINOR
#define ONYX_FW_MINOR 0
#endif
#ifndef ONYX_FW_PATCH
#define ONYX_FW_PATCH 0
#endif
#ifndef ONYX_PRODUCT_ID
#define ONYX_PRODUCT_ID 0x0601
#endif
#ifndef ONYX_HW_REV
#define ONYX_HW_REV 1
#endif

// ---------------- Brand Discovery Identifiers ----------------
// Custom 128-bit Brand / OTA Service UUID:
//   Service : 4f4e5958-5336-4f54-4100-000000000001 ("ONYX-S6-OTA-0001")
//   Info    : 4f4e5958-5336-4f54-4100-000000000002 (Read | Notify)
//   Control : 4f4e5958-5336-4f54-4100-000000000003 (Write | Notify)
//   Data    : 4f4e5958-5336-4f54-4100-000000000004 (Write | WriteNoRsp)
//
// In BLE little-endian byte order (LSB first, as required by ESP-IDF Bluedroid):
constexpr uint8_t UUID_SERVICE_LE[16] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x54, 0x4f, 0x36, 0x53, 0x58, 0x59, 0x4e, 0x4f
};
constexpr uint8_t UUID_CHAR_INFO_LE[16] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x54, 0x4f, 0x36, 0x53, 0x58, 0x59, 0x4e, 0x4f
};
constexpr uint8_t UUID_CHAR_CTRL_LE[16] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x54, 0x4f, 0x36, 0x53, 0x58, 0x59, 0x4e, 0x4f
};
constexpr uint8_t UUID_CHAR_DATA_LE[16] = {
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41,
    0x54, 0x4f, 0x36, 0x53, 0x58, 0x59, 0x4e, 0x4f
};

// Custom Manufacturer Specific Data (BLE Adv + Classic EIR)
// Format (8 bytes total): [0x4F, 0x59, 'O', 'S', modelLo, fwMajor, fwMinor, fwPatch]
// Company ID = 0x594F ("OY" little-endian)
constexpr uint16_t MANUFACTURER_COMPANY_ID = 0x594F;
constexpr uint8_t  MFG_MAGIC_0             = 0x4F; // 'O'
constexpr uint8_t  MFG_MAGIC_1             = 0x53; // 'S'

// Classic Bluetooth SPP Service Name (SDP record)
constexpr const char *SPP_SERVICE_NAME = "ONYX_S6_OTA";

// Protocol version & framing sync
constexpr uint8_t  PROTOCOL_VERSION = 1;
constexpr uint8_t  FRAME_SYNC_0     = 0xAA;
constexpr uint8_t  FRAME_SYNC_1     = 0x55;
constexpr uint16_t MAX_FRAME_PAYLOAD = 540; // [4B offset + up to 512B data] + margin

// ---------------- Embedded Binary Firmware Descriptor ----------------
// Placed in .rodata with __attribute__((used)) so both the Qt6 app and ESP32
// can verify that a .bin file truly belongs to an Onyx Studio firmware build.
constexpr char FW_DESC_MAGIC[16] = "ONYX_FW_DESC_V1";

struct __attribute__((packed)) FwDescriptor {
  char     magic[16];     // "ONYX_FW_DESC_V1\0"
  uint16_t productId;     // e.g. 0x0601 = HK Onyx Studio 6
  uint8_t  hwRev;         // Hardware revision
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint8_t  fwPatch;
  uint16_t flags;         // Reserved (0)
  char     modelName[32]; // Null-terminated model string
  char     buildDate[16]; // __DATE__
  char     buildTime[16]; // __TIME__
};

// ---------------- Wire Commands & Responses ----------------
enum MsgType : uint8_t {
  // Host -> Speaker
  CMD_HELLO         = 0x01,
  CMD_OTA_BEGIN     = 0x02,
  CMD_OTA_DATA      = 0x03, // Payload: [uint32 offset][chunk bytes]
  CMD_OTA_COMMIT    = 0x04,
  CMD_OTA_REBOOT    = 0x05,
  CMD_OTA_ABORT     = 0x06,

  // Speaker -> Host
  RSP_DEVICE_INFO   = 0x81,
  RSP_ACK           = 0x82,
  EVT_PROGRESS      = 0x83,
  RSP_ERROR         = 0x84,
};

enum OtaState : uint8_t {
  STATE_IDLE      = 0,
  STATE_RECEIVING = 1,
  STATE_VERIFYING = 2,
  STATE_COMMITTED = 3,
  STATE_REBOOTING = 4,
  STATE_ERROR     = 5,
};

enum ErrorCode : uint8_t {
  ERR_NONE             = 0x00,
  ERR_BAD_FRAME        = 0x01,
  ERR_BAD_STATE        = 0x02,
  ERR_MODEL_MISMATCH   = 0x03,
  ERR_IMAGE_TOO_LARGE  = 0x04,
  ERR_LOW_BATTERY      = 0x05,
  ERR_OFFSET_MISMATCH  = 0x06,
  ERR_FLASH_WRITE      = 0x07,
  ERR_SHA256_MISMATCH  = 0x08,
  ERR_INVALID_IMAGE    = 0x09,
  ERR_OTA_BEGIN_FAILED = 0x0A,
  ERR_OTA_END_FAILED   = 0x0B,
  ERR_BUSY_OTHER_LINK  = 0x0C,
};

// Capabilities / status bitmask in DeviceInfoPayload::statusFlags
constexpr uint16_t STATUS_DC_PRESENT      = (1u << 0);
constexpr uint16_t STATUS_CHARGING        = (1u << 1);
constexpr uint16_t STATUS_LOW_BATTERY     = (1u << 2);
constexpr uint16_t STATUS_BT_AUDIO_CONN   = (1u << 3);
constexpr uint16_t STATUS_AUX_ACTIVE      = (1u << 4);
constexpr uint16_t STATUS_SPP_AVAILABLE   = (1u << 5);
constexpr uint16_t STATUS_BLE_AVAILABLE   = (1u << 6);
constexpr uint16_t STATUS_ROLLBACK_VALID  = (1u << 7);

// Payload for RSP_DEVICE_INFO and BLE DEVICE_INFO characteristic (96 bytes)
struct __attribute__((packed)) DeviceInfoPayload {
  char     magic[4];        // "ONYX"
  uint8_t  protocolVersion; // PROTOCOL_VERSION
  uint8_t  hwRev;
  uint16_t productId;       // ONYX_PRODUCT_ID
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint8_t  fwPatch;
  uint8_t  runningSlot;     // 0 = ota_0, 1 = ota_1, 0xFF = factory/other
  uint32_t maxOtaSize;      // Size of inactive OTA partition in bytes
  uint16_t batteryMv;       // Battery voltage in millivolts
  uint8_t  batteryPct;      // 0..100 estimated Li-ion %
  uint8_t  otaState;        // OtaState
  uint16_t statusFlags;     // STATUS_* bitmask
  uint16_t negotiatedChunk; // Recommended chunk payload size
  uint8_t  classicMac[6];   // Speaker's Bluetooth Classic BD_ADDR
  uint8_t  windowPackets;   // Recommended packets per ACK window
  uint8_t  reserved;
  char     modelName[32];   // Null-terminated model name
  char     buildDate[16];   // Null-terminated build date
  char     buildTime[16];   // Null-terminated build time
};

// Payload for CMD_OTA_BEGIN (44 bytes)
struct __attribute__((packed)) OtaBeginPayload {
  uint32_t imageSize;
  uint8_t  sha256[32];
  uint16_t productId;
  uint8_t  fwMajor;
  uint8_t  fwMinor;
  uint8_t  fwPatch;
  uint8_t  flags;           // bit 0: force allow same/older version
  uint16_t reserved;
};

// Payload for RSP_ACK (12 bytes)
struct __attribute__((packed)) AckPayload {
  uint8_t  origCmd;
  uint8_t  statusCode;      // ErrorCode (ERR_NONE on success)
  uint16_t chunkSize;       // Negotiated chunk size (on BEGIN) or window size
  uint32_t nextOffset;      // Next expected byte offset
  uint32_t detail;          // Extra info (e.g., window size on BEGIN)
};

// Payload for EVT_PROGRESS (12 bytes)
struct __attribute__((packed)) ProgressPayload {
  uint32_t bytesReceived;
  uint32_t totalBytes;
  uint8_t  otaState;
  uint8_t  percent;
  uint16_t reserved;
};

// Payload for RSP_ERROR (8 + message bytes)
struct __attribute__((packed)) ErrorPayload {
  uint8_t  origCmd;
  uint8_t  errCode;
  uint16_t reserved;
  uint32_t expectedOffset;
};

// ---------------- CRC-16/CCITT-FALSE ----------------
inline uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000) crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      else              crc = static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

} // namespace OnyxOta
