#include "OtaProtocol.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <cstring>

namespace OnyxOta {

quint16 crc16Ccitt(const quint8 *data, qsizetype len) {
    quint16 crc = 0xFFFF;
    for (qsizetype i = 0; i < len; ++i) {
        crc ^= static_cast<quint16>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000)
                crc = static_cast<quint16>((crc << 1) ^ 0x1021);
            else
                crc = static_cast<quint16>(crc << 1);
        }
    }
    return crc;
}

QByteArray encodeFrame(quint8 msgType, const QByteArray &payload, quint8 flags) {
    QByteArray out;
    const quint16 len = static_cast<quint16>(payload.size());
    out.reserve(len + 8);
    out.append(static_cast<char>(kFrameSync0));
    out.append(static_cast<char>(kFrameSync1));
    out.append(static_cast<char>(msgType));
    out.append(static_cast<char>(flags));
    out.append(static_cast<char>(len & 0xFF));
    out.append(static_cast<char>((len >> 8) & 0xFF));
    out.append(payload);

    const quint16 crc = crc16Ccitt(reinterpret_cast<const quint8 *>(out.constData() + 2), len + 4);
    out.append(static_cast<char>(crc & 0xFF));
    out.append(static_cast<char>((crc >> 8) & 0xFF));
    return out;
}

bool decodeSingleFrame(const QByteArray &raw, DecodedFrame &out) {
    if (raw.size() < 8) return false;
    const auto *d = reinterpret_cast<const quint8 *>(raw.constData());
    if (d[0] != kFrameSync0 || d[1] != kFrameSync1) return false;

    const quint16 len = static_cast<quint16>(d[4]) | (static_cast<quint16>(d[5]) << 8);
    if (len > kMaxFramePayload || raw.size() != len + 8) return false;

    const quint16 rxCrc   = static_cast<quint16>(d[6 + len]) | (static_cast<quint16>(d[7 + len]) << 8);
    const quint16 calcCrc = crc16Ccitt(d + 2, len + 4);
    if (rxCrc != calcCrc) return false;

    out.msgType = d[2];
    out.flags   = d[3];
    out.payload = raw.mid(6, len);
    return true;
}

void FrameStreamParser::reset() {
    m_state  = State::Sync0;
    m_hdrIdx = 0;
    m_payload.clear();
}

QList<DecodedFrame> FrameStreamParser::feed(const QByteArray &bytes) {
    QList<DecodedFrame> frames;
    for (char ch : bytes) {
        const quint8 b = static_cast<quint8>(ch);
        switch (m_state) {
        case State::Sync0:
            if (b == kFrameSync0) m_state = State::Sync1;
            break;
        case State::Sync1:
            if (b == kFrameSync1) {
                m_hdrIdx = 0;
                m_state  = State::Header;
            } else if (b != kFrameSync0) {
                m_state = State::Sync0;
            }
            break;
        case State::Header:
            m_hdr[m_hdrIdx++] = b;
            if (m_hdrIdx == 4) {
                m_msgType = m_hdr[0];
                m_flags   = m_hdr[1];
                m_length  = static_cast<quint16>(m_hdr[2]) | (static_cast<quint16>(m_hdr[3]) << 8);
                m_payload.clear();
                if (m_length > kMaxFramePayload) {
                    reset();
                } else if (m_length == 0) {
                    m_state = State::CrcLo;
                } else {
                    m_payload.reserve(m_length);
                    m_state = State::Payload;
                }
            }
            break;
        case State::Payload:
            m_payload.append(static_cast<char>(b));
            if (m_payload.size() >= m_length) {
                m_state = State::CrcLo;
            }
            break;
        case State::CrcLo:
            m_rxCrc = b;
            m_state = State::CrcHi;
            break;
        case State::CrcHi: {
            m_rxCrc |= static_cast<quint16>(b) << 8;
            QByteArray crcBuf;
            crcBuf.reserve(4 + m_payload.size());
            crcBuf.append(reinterpret_cast<const char *>(m_hdr), 4);
            crcBuf.append(m_payload);
            const quint16 calc = crc16Ccitt(reinterpret_cast<const quint8 *>(crcBuf.constData()), crcBuf.size());
            if (calc == m_rxCrc) {
                DecodedFrame f;
                f.msgType = m_msgType;
                f.flags   = m_flags;
                f.payload = m_payload;
                frames.append(f);
            }
            reset();
            break;
        }
        }
    }
    return frames;
}

FirmwareImageInfo parseAndValidateFirmwareFile(const QUrl &fileUrl) {
    FirmwareImageInfo info;

    // On Android, FileDialog returns a content:// URI that QFile opens natively via toString().
    // On Desktop, toLocalFile() resolves file:// URLs cleanly.
    const QString path = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileUrl.toString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        info.errorText = QStringLiteral("Cannot open firmware file: %1").arg(file.errorString());
        return info;
    }

    const QByteArray data = file.readAll();
    file.close();

    QString displayFile = QUrl::fromPercentEncoding(path.toUtf8());
    const qsizetype slashPos = displayFile.lastIndexOf(QLatin1Char('/'));
    if (slashPos >= 0) displayFile = displayFile.mid(slashPos + 1);
    const qsizetype colonPos = displayFile.lastIndexOf(QLatin1Char(':'));
    if (colonPos >= 0) displayFile = displayFile.mid(colonPos + 1);
    info.fileName  = displayFile.isEmpty() ? QFileInfo(path).fileName() : displayFile;
    info.sizeBytes = static_cast<quint32>(data.size());

    if (data.size() < 4096) {
        info.errorText = QStringLiteral("File is too small (%1 bytes) to be an ESP32 firmware image.").arg(data.size());
        return info;
    }

    // Check ESP32 application image magic byte (0xE9)
    if (static_cast<quint8>(data[0]) != 0xE9) {
        info.errorText = QStringLiteral("Invalid ESP32 binary (missing 0xE9 image header magic).");
        return info;
    }

    // Locate embedded OnyxFwDescriptor ("ONYX_FW_DESC_V1\0")
    const QByteArray magicNeedle(kFwDescMagic, sizeof(kFwDescMagic));
    const qsizetype idx = data.indexOf(magicNeedle);
    if (idx < 0 || idx + static_cast<qsizetype>(sizeof(FwDescriptor)) > data.size()) {
        info.errorText = QStringLiteral("Not an Onyx Studio firmware binary (missing ONYX_FW_DESC_V1 signature).");
        return info;
    }

    FwDescriptor desc{};
    std::memcpy(&desc, data.constData() + idx, sizeof(desc));

    info.productId = desc.productId;
    info.hwRev     = desc.hwRev;
    info.fwMajor   = desc.fwMajor;
    info.fwMinor   = desc.fwMinor;
    info.fwPatch   = desc.fwPatch;
    info.modelName = QString::fromLatin1(desc.modelName, strnlen(desc.modelName, sizeof(desc.modelName)));
    info.buildDate = QString::fromLatin1(desc.buildDate, strnlen(desc.buildDate, sizeof(desc.buildDate)));
    info.buildTime = QString::fromLatin1(desc.buildTime, strnlen(desc.buildTime, sizeof(desc.buildTime)));
    info.sha256    = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    info.rawData   = data;
    info.valid     = true;
    return info;
}

QString formatMacAddress(const uint8_t mac[6]) {
    return QStringLiteral("%1:%2:%3:%4:%5:%6")
        .arg(mac[0], 2, 16, QLatin1Char('0'))
        .arg(mac[1], 2, 16, QLatin1Char('0'))
        .arg(mac[2], 2, 16, QLatin1Char('0'))
        .arg(mac[3], 2, 16, QLatin1Char('0'))
        .arg(mac[4], 2, 16, QLatin1Char('0'))
        .arg(mac[5], 2, 16, QLatin1Char('0'))
        .toUpper();
}

QString errorCodeToString(quint8 code) {
    switch (code) {
    case ERR_NONE:             return QStringLiteral("OK");
    case ERR_BAD_FRAME:        return QStringLiteral("Malformed protocol frame");
    case ERR_BAD_STATE:        return QStringLiteral("Invalid OTA state");
    case ERR_MODEL_MISMATCH:   return QStringLiteral("Firmware model does not match speaker");
    case ERR_IMAGE_TOO_LARGE:  return QStringLiteral("Firmware exceeds OTA partition size");
    case ERR_LOW_BATTERY:      return QStringLiteral("Speaker battery too low — connect AC power");
    case ERR_OFFSET_MISMATCH:  return QStringLiteral("Chunk offset mismatch (resyncing)");
    case ERR_FLASH_WRITE:      return QStringLiteral("SPI flash write error");
    case ERR_SHA256_MISMATCH:  return QStringLiteral("SHA-256 checksum mismatch");
    case ERR_INVALID_IMAGE:    return QStringLiteral("Invalid ESP32 firmware image");
    case ERR_OTA_BEGIN_FAILED: return QStringLiteral("Failed to prepare OTA partition");
    case ERR_OTA_END_FAILED:   return QStringLiteral("Firmware validation or boot switch failed");
    case ERR_BUSY_OTHER_LINK:  return QStringLiteral("Speaker is already updating over another link");
    default:                   return QStringLiteral("Unknown error (0x%1)").arg(code, 2, 16, QLatin1Char('0'));
    }
}

} // namespace OnyxOta
