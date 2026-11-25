#pragma once

#include <QBluetoothUuid>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <cstdint>

namespace OnyxOta {

// Brand & GATT UUIDs (must match ESP32 include/onyx_ota_protocol.h exactly)
inline const QBluetoothUuid kServiceUuid{QStringLiteral("{4f4e5958-5336-4f54-4100-000000000001}")};
inline const QBluetoothUuid kCharInfoUuid{QStringLiteral("{4f4e5958-5336-4f54-4100-000000000002}")};
inline const QBluetoothUuid kCharCtrlUuid{QStringLiteral("{4f4e5958-5336-4f54-4100-000000000003}")};
inline const QBluetoothUuid kCharDataUuid{QStringLiteral("{4f4e5958-5336-4f54-4100-000000000004}")};

constexpr quint16 kManufacturerCompanyId = 0x594F; // "OY"
constexpr quint8  kMfgMagic0             = 0x4F;   // 'O'
constexpr quint8  kMfgMagic1             = 0x53;   // 'S'
inline const QString kSppServiceName     = QStringLiteral("ONYX_S6_OTA");

constexpr quint8  kProtocolVersion       = 1;
constexpr quint8  kFrameSync0            = 0xAA;
constexpr quint8  kFrameSync1            = 0x55;
constexpr quint16 kMaxFramePayload       = 540;

constexpr char kFwDescMagic[16]          = "ONYX_FW_DESC_V1";

enum MsgType : quint8 {
    CMD_HELLO       = 0x01,
    CMD_OTA_BEGIN   = 0x02,
    CMD_OTA_DATA    = 0x03,
    CMD_OTA_COMMIT  = 0x04,
    CMD_OTA_REBOOT  = 0x05,
    CMD_OTA_ABORT   = 0x06,

    RSP_DEVICE_INFO = 0x81,
    RSP_ACK         = 0x82,
    EVT_PROGRESS    = 0x83,
    RSP_ERROR       = 0x84,
};

enum OtaState : quint8 {
    STATE_IDLE      = 0,
    STATE_RECEIVING = 1,
    STATE_VERIFYING = 2,
    STATE_COMMITTED = 3,
    STATE_REBOOTING = 4,
    STATE_ERROR     = 5,
};

enum ErrorCode : quint8 {
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

constexpr quint16 STATUS_DC_PRESENT     = (1u << 0);
constexpr quint16 STATUS_CHARGING       = (1u << 1);
constexpr quint16 STATUS_LOW_BATTERY    = (1u << 2);
constexpr quint16 STATUS_BT_AUDIO_CONN  = (1u << 3);
constexpr quint16 STATUS_AUX_ACTIVE     = (1u << 4);
constexpr quint16 STATUS_SPP_AVAILABLE  = (1u << 5);
constexpr quint16 STATUS_BLE_AVAILABLE  = (1u << 6);
constexpr quint16 STATUS_ROLLBACK_VALID = (1u << 7);

#pragma pack(push, 1)
struct FwDescriptor {
    char     magic[16];
    uint16_t productId;
    uint8_t  hwRev;
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint16_t flags;
    char     modelName[32];
    char     buildDate[16];
    char     buildTime[16];
};

struct DeviceInfoPayload {
    char     magic[4];
    uint8_t  protocolVersion;
    uint8_t  hwRev;
    uint16_t productId;
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint8_t  runningSlot;
    uint32_t maxOtaSize;
    uint16_t batteryMv;
    uint8_t  batteryPct;
    uint8_t  otaState;
    uint16_t statusFlags;
    uint16_t negotiatedChunk;
    uint8_t  classicMac[6];
    uint8_t  windowPackets;
    uint8_t  reserved;
    char     modelName[32];
    char     buildDate[16];
    char     buildTime[16];
};

struct OtaBeginPayload {
    uint32_t imageSize;
    uint8_t  sha256[32];
    uint16_t productId;
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint8_t  flags;
    uint16_t reserved;
};

struct AckPayload {
    uint8_t  origCmd;
    uint8_t  statusCode;
    uint16_t chunkSize;
    uint32_t nextOffset;
    uint32_t detail;
};

struct ProgressPayload {
    uint32_t bytesReceived;
    uint32_t totalBytes;
    uint8_t  otaState;
    uint8_t  percent;
    uint16_t reserved;
};

struct ErrorPayload {
    uint8_t  origCmd;
    uint8_t  errCode;
    uint16_t reserved;
    uint32_t expectedOffset;
};
#pragma pack(pop)

struct DecodedFrame {
    quint8     msgType = 0;
    quint8     flags   = 0;
    QByteArray payload;
};

struct FirmwareImageInfo {
    bool       valid         = false;
    QString    errorText;
    QString    fileName;
    QString    modelName;
    quint16    productId     = 0;
    quint8     hwRev         = 0;
    quint8     fwMajor       = 0;
    quint8     fwMinor       = 0;
    quint8     fwPatch       = 0;
    QString    buildDate;
    QString    buildTime;
    quint32    sizeBytes     = 0;
    QByteArray sha256;
    QByteArray rawData;

    QString versionString() const {
        return QStringLiteral("v%1.%2.%3").arg(fwMajor).arg(fwMinor).arg(fwPatch);
    }
};

quint16    crc16Ccitt(const quint8 *data, qsizetype len);
QByteArray encodeFrame(quint8 msgType, const QByteArray &payload = {}, quint8 flags = 0);
bool       decodeSingleFrame(const QByteArray &raw, DecodedFrame &out);

class FrameStreamParser {
public:
    void reset();
    QList<DecodedFrame> feed(const QByteArray &bytes);

private:
    enum class State { Sync0, Sync1, Header, Payload, CrcLo, CrcHi };
    State      m_state   = State::Sync0;
    quint8     m_hdr[4]  = {};
    int        m_hdrIdx  = 0;
    quint8     m_msgType = 0;
    quint8     m_flags   = 0;
    quint16    m_length  = 0;
    QByteArray m_payload;
    quint16    m_rxCrc   = 0;
};

FirmwareImageInfo parseAndValidateFirmwareFile(const QUrl &fileUrl);
QString           formatMacAddress(const uint8_t mac[6]);
QString           errorCodeToString(quint8 code);

} // namespace OnyxOta
