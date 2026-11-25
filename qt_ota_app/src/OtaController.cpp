#include "OtaController.h"
#include "BleOtaTransport.h"
#include "ClassicOtaTransport.h"

#include <QBluetoothPermission>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <algorithm>
#include <cstring>

OtaController::OtaController(QObject *parent)
    : QObject(parent)
    , m_scanner(new SpeakerScanner(this))
{
    m_blePaceTimer.setInterval(8);
    connect(&m_blePaceTimer, &QTimer::timeout, this, &OtaController::onBlePaceTick);

    m_watchdogTimer.setSingleShot(true);
    m_watchdogTimer.setInterval(6000);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &OtaController::onWatchdogTimeout);

    m_handshakeTimer.setSingleShot(true);
    m_handshakeTimer.setInterval(5000);
    connect(&m_handshakeTimer, &QTimer::timeout, this, &OtaController::onHandshakeTimeout);

    connect(m_scanner, &SpeakerScanner::scanError, this, [this](const QString &msg) {
        appendLog(QStringLiteral("Scan error: %1").arg(msg));
    });

    // Re-check permissions when returning from Android system Settings
    if (auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(guiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !m_btPermGranted) {
                requestBluetoothPermissions();
            }
        });
    }

    // Check current status synchronously (no dialog yet), then request once Activity is active
#if QT_CONFIG(permissions)
    {
        QBluetoothPermission btPerm;
        btPerm.setCommunicationModes(QBluetoothPermission::Access);
        m_btPermGranted = (qApp->checkPermission(btPerm) == Qt::PermissionStatus::Granted);
        m_permStatus = m_btPermGranted
                           ? QStringLiteral("Bluetooth permissions granted")
                           : QStringLiteral("Waiting for Nearby Devices permission...");
    }
#endif

    // Defer initial runtime permission request until the Android Activity event loop is active
    QTimer::singleShot(250, this, &OtaController::requestBluetoothPermissions);
}

OtaController::~OtaController() {
    teardownTransport();
}

void OtaController::requestBluetoothPermissions() {
#if QT_CONFIG(permissions)
    QBluetoothPermission btPerm;
    btPerm.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(btPerm)) {
    case Qt::PermissionStatus::Granted:
        m_btPermGranted = true;
        m_permStatus    = QStringLiteral("Bluetooth permissions granted");
        emit permissionChanged();
        break;
    case Qt::PermissionStatus::Undetermined:
        m_btPermGranted = false;
        m_permStatus    = QStringLiteral("Requesting Bluetooth permission...");
        emit permissionChanged();
        qApp->requestPermission(btPerm, this, [this](const QPermission &perm) {
            m_btPermGranted = (perm.status() == Qt::PermissionStatus::Granted);
            m_permStatus = m_btPermGranted
                               ? QStringLiteral("Bluetooth permissions granted")
                               : QStringLiteral("Bluetooth permission denied — grant it in system settings");
            emit permissionChanged();
            if (m_btPermGranted) {
                startScan();
            }
        });
        break;
    case Qt::PermissionStatus::Denied:
        m_btPermGranted = false;
        m_permStatus    = QStringLiteral("Bluetooth permission denied — enable in system settings");
        emit permissionChanged();
        break;
    }
#else
    m_btPermGranted = true;
    m_permStatus    = QStringLiteral("Bluetooth ready");
    emit permissionChanged();
#endif
}

void OtaController::startScan() {
    if (!m_btPermGranted) {
        requestBluetoothPermissions();
        return;
    }
    appendLog(QStringLiteral("Scanning BLE & Classic for Onyx Studio speakers..."));
    m_scanner->startScan();
}

void OtaController::setTransportPreference(int pref) {
    if (pref < PreferAuto || pref > PreferBle) return;
    if (m_transportPref != pref) {
        m_transportPref = pref;
        emit transportPreferenceChanged();
    }
}

QString OtaController::activeTransportName() const {
    return m_transport ? m_transport->kindName() : QStringLiteral("Disconnected");
}

bool OtaController::canSwitchToClassic() const {
    if (!m_verified || m_otaInProgress || !m_transport) return false;
    return m_transport->kind() == OtaTransport::Kind::Ble &&
           m_selectedSpeaker.hasClassic &&
           !m_selectedSpeaker.classicAddress.isNull() &&
           (m_statusFlags & OnyxOta::STATUS_SPP_AVAILABLE);
}

bool OtaController::canSwitchToBle() const {
    if (!m_verified || m_otaInProgress || !m_transport) return false;
    return m_transport->kind() == OtaTransport::Kind::ClassicSpp &&
           m_selectedSpeaker.hasBle &&
           (m_statusFlags & OnyxOta::STATUS_BLE_AVAILABLE);
}

QString OtaController::fileBuildInfo() const {
    if (!m_fwInfo.valid) return {};
    return QStringLiteral("%1 %2").arg(m_fwInfo.buildDate, m_fwInfo.buildTime);
}

QString OtaController::fileSha256Short() const {
    if (!m_fwInfo.valid || m_fwInfo.sha256.isEmpty()) return {};
    return QString::fromLatin1(m_fwInfo.sha256.toHex().left(16)).toUpper() + QStringLiteral("...");
}

bool OtaController::fileCompatibleWithSpeaker() const {
    if (!m_fwInfo.valid) return false;
    if (!m_verified) return true; // Valid Onyx binary; speaker check runs once connected
    if (m_fwInfo.productId != m_speakerProductId) return false;
    if (m_maxOtaSize > 0 && m_fwInfo.sizeBytes > m_maxOtaSize) return false;
    return true;
}

QString OtaController::fileCompatibilityText() const {
    if (!m_fileLoaded) return QStringLiteral("No firmware file selected.");
    if (!m_fwInfo.valid) return m_fwInfo.errorText;
    if (!m_verified) {
        return QStringLiteral("Valid Onyx binary (%1 %2). Connect speaker to verify target slot.")
            .arg(m_fwInfo.modelName, m_fwInfo.versionString());
    }
    if (m_fwInfo.productId != m_speakerProductId) {
        return QStringLiteral("Blocked: firmware product ID 0x%1 does not match speaker 0x%2.")
            .arg(m_fwInfo.productId, 4, 16, QLatin1Char('0'))
            .arg(m_speakerProductId, 4, 16, QLatin1Char('0'));
    }
    if (m_maxOtaSize > 0 && m_fwInfo.sizeBytes > m_maxOtaSize) {
        return QStringLiteral("Blocked: file (%1 KB) exceeds OTA partition (%2 KB).")
            .arg(m_fwInfo.sizeBytes / 1024)
            .arg(m_maxOtaSize / 1024);
    }

    const quint32 newVer = (quint32(m_fwInfo.fwMajor) << 16) | (quint32(m_fwInfo.fwMinor) << 8) | m_fwInfo.fwPatch;
    const quint32 curVer = (quint32(m_speakerMajor) << 16) | (quint32(m_speakerMinor) << 8) | m_speakerPatch;
    if (newVer > curVer) {
        return QStringLiteral("Compatible upgrade: %1 -> %2").arg(m_speakerVersion, m_fwInfo.versionString());
    }
    if (newVer == curVer) {
        return QStringLiteral("Compatible reinstall: same version (%1)").arg(m_fwInfo.versionString());
    }
    return QStringLiteral("Compatible downgrade: %1 -> %2").arg(m_speakerVersion, m_fwInfo.versionString());
}

bool OtaController::canStartOta() const {
    return m_verified && !m_otaInProgress && m_fwInfo.valid && fileCompatibleWithSpeaker();
}

void OtaController::connectToDevice(const QString &deviceId) {
    const SpeakerDevice *dev = m_scanner->findById(deviceId);
    if (!dev) {
        appendLog(QStringLiteral("Selected speaker is no longer in scan list."));
        return;
    }

    m_scanner->stopScan();
    m_selectedSpeaker = *dev;

    OtaTransport::Kind chosen = OtaTransport::Kind::Ble;
    if (m_transportPref == PreferClassic && dev->hasClassic) {
        chosen = OtaTransport::Kind::ClassicSpp;
    } else if (m_transportPref == PreferBle && dev->hasBle) {
        chosen = OtaTransport::Kind::Ble;
    } else {
        // Auto: prefer fast Classic SPP if already known/paired, else BLE GATT
        if (dev->hasClassic && dev->pairedClassic) {
            chosen = OtaTransport::Kind::ClassicSpp;
        } else if (dev->hasBle) {
            chosen = OtaTransport::Kind::Ble;
        } else {
            chosen = OtaTransport::Kind::ClassicSpp;
        }
    }

    openTransport(chosen);
}

void OtaController::switchTransport(int targetKind) {
    if (!m_verified || m_otaInProgress) return;
    if (targetKind == 1 && canSwitchToClassic()) {
        appendLog(QStringLiteral("Switching link to fast Bluetooth Classic SPP..."));
        openTransport(OtaTransport::Kind::ClassicSpp);
    } else if (targetKind == 2 && canSwitchToBle()) {
        appendLog(QStringLiteral("Switching link to Bluetooth Low Energy (BLE GATT)..."));
        openTransport(OtaTransport::Kind::Ble);
    }
}

void OtaController::openTransport(OtaTransport::Kind kind) {
    teardownTransport();

    m_connecting   = true;
    m_verified     = false;
    m_expectReboot = false;
    emit connectionStateChanged();
    emit otaStateChanged();

    if (kind == OtaTransport::Kind::ClassicSpp) {
        m_transport = new ClassicOtaTransport(this);
    } else {
        m_transport = new BleOtaTransport(this);
    }

    connect(m_transport, &OtaTransport::transportReady,
            this, &OtaController::onTransportReady);
    connect(m_transport, &OtaTransport::disconnected,
            this, &OtaController::onTransportDisconnected);
    connect(m_transport, &OtaTransport::errorOccurred,
            this, &OtaController::onTransportError);
    connect(m_transport, &OtaTransport::controlFrameReceived,
            this, &OtaController::onControlFrameReceived);
    connect(m_transport, &OtaTransport::deviceInfoSnapshot,
            this, &OtaController::onDeviceInfoSnapshot);

    setOtaPhase(QStringLiteral("Connecting"),
                QStringLiteral("Connecting to %1 over %2...")
                    .arg(m_selectedSpeaker.name, m_transport->kindName()),
                false);
    appendLog(QStringLiteral("Connecting to %1 via %2...").arg(m_selectedSpeaker.name, m_transport->kindName()));

    m_transport->connectToSpeaker(m_selectedSpeaker);
}

void OtaController::disconnectSpeaker() {
    if (m_otaInProgress) {
        abortOta();
    }
    teardownTransport();
    m_connecting = false;
    m_verified   = false;
    setOtaPhase(QStringLiteral("Idle"), QStringLiteral("Disconnected."), false);
    emit connectionStateChanged();
    emit speakerInfoChanged();
    emit firmwareFileChanged();
    emit otaStateChanged();
}

void OtaController::teardownTransport() {
    m_blePaceTimer.stop();
    m_watchdogTimer.stop();
    m_handshakeTimer.stop();

    if (m_transport) {
        m_transport->disconnect(this);
        m_transport->disconnectFromSpeaker();
        m_transport->deleteLater();
        m_transport = nullptr;
    }
}

void OtaController::refreshSpeakerInfo() {
    if (m_transport && m_transport->isConnected()) {
        m_transport->sendControlFrame(OnyxOta::CMD_HELLO);
    }
}

void OtaController::loadFirmwareFile(const QUrl &fileUrl) {
    if (fileUrl.isEmpty()) return;
    m_fileLoaded = true;
    m_fwInfo = OnyxOta::parseAndValidateFirmwareFile(fileUrl);

    if (m_fwInfo.valid) {
        appendLog(QStringLiteral("Loaded %1 (%2 %3, %4 KB, SHA-256 %5)")
                      .arg(m_fwInfo.fileName, m_fwInfo.modelName, m_fwInfo.versionString())
                      .arg(m_fwInfo.sizeBytes / 1024)
                      .arg(fileSha256Short()));
    } else {
        appendLog(QStringLiteral("Firmware file rejected: %1").arg(m_fwInfo.errorText));
    }

    emit firmwareFileChanged();
    emit otaStateChanged();
}

void OtaController::startOta() {
    if (!canStartOta() || !m_transport) return;

    m_nextOffset   = 0;
    m_ackedOffset  = 0;
    m_windowLeft   = 0;
    m_retryCount   = 0;
    m_progress     = 0.0;
    m_speedKBps    = 0.0;
    m_etaSeconds   = 0;
    m_expectReboot = false;
    m_transferTimer.start();

    OnyxOta::OtaBeginPayload req{};
    req.imageSize = m_fwInfo.sizeBytes;
    std::memcpy(req.sha256, m_fwInfo.sha256.constData(), 32);
    req.productId = m_fwInfo.productId;
    req.fwMajor   = m_fwInfo.fwMajor;
    req.fwMinor   = m_fwInfo.fwMinor;
    req.fwPatch   = m_fwInfo.fwPatch;
    req.flags     = 0x01; // Allow upgrade/reinstall/downgrade explicitly selected by user

    setOtaPhase(QStringLiteral("Preparing"),
                QStringLiteral("Erasing inactive OTA partition on %1...").arg(m_speakerModel),
                true);
    emit otaProgressChanged();

    appendLog(QStringLiteral("Starting OTA over %1 (%2 -> %3, %4 bytes)")
                  .arg(m_transport->kindName(), m_speakerVersion, m_fwInfo.versionString())
                  .arg(m_fwInfo.sizeBytes));

    const QByteArray payload(reinterpret_cast<const char *>(&req), sizeof(req));
    m_transport->sendControlFrame(OnyxOta::CMD_OTA_BEGIN, payload);
    m_watchdogTimer.start(10000); // Partition erase on flash can take 2-4 seconds
}

void OtaController::abortOta() {
    m_blePaceTimer.stop();
    m_watchdogTimer.stop();
    if (m_transport && m_transport->isConnected()) {
        m_transport->sendControlFrame(OnyxOta::CMD_OTA_ABORT);
    }
    setOtaPhase(QStringLiteral("Aborted"), QStringLiteral("OTA update cancelled — audio restored."), false);
    appendLog(QStringLiteral("OTA aborted by user."));
}

void OtaController::clearLogs() {
    m_logs.clear();
    emit logsChanged();
}

void OtaController::onTransportReady() {
    appendLog(QStringLiteral("Link up (%1). Verifying Onyx Studio handshake...").arg(m_transport->kindName()));
    m_handshakeTimer.start();
    m_transport->sendControlFrame(OnyxOta::CMD_HELLO);
}

void OtaController::onHandshakeTimeout() {
    if (!m_verified) {
        appendLog(QStringLiteral("Handshake timed out — device did not answer as an Onyx Studio speaker."));
        teardownTransport();
        m_connecting = false;
        setOtaPhase(QStringLiteral("Error"),
                    QStringLiteral("Handshake failed — device is not running Onyx OTA firmware."),
                    false);
        emit connectionStateChanged();
    }
}

void OtaController::onTransportDisconnected() {
    m_blePaceTimer.stop();
    m_watchdogTimer.stop();
    m_handshakeTimer.stop();

    m_connecting = false;
    m_verified   = false;
    teardownTransport();

    if (m_expectReboot) {
        m_expectReboot = false;
        setOtaPhase(QStringLiteral("Complete"),
                    QStringLiteral("Firmware %1 flashed! Speaker is rebooting — tap Scan in ~4 s to verify.")
                        .arg(m_fwInfo.versionString()),
                    false);
        appendLog(QStringLiteral("Speaker disconnected for reboot into %1.").arg(m_fwInfo.versionString()));
    } else if (m_otaInProgress) {
        finishOtaWithError(QStringLiteral("Bluetooth link dropped during OTA (active slot remains safe)."));
    } else {
        setOtaPhase(QStringLiteral("Idle"), QStringLiteral("Speaker disconnected."), false);
        appendLog(QStringLiteral("Speaker disconnected."));
    }

    emit connectionStateChanged();
    emit speakerInfoChanged();
    emit firmwareFileChanged();
}

void OtaController::onTransportError(const QString &message) {
    appendLog(message);
    if (m_otaInProgress) {
        finishOtaWithError(message);
    } else {
        teardownTransport();
        m_connecting = false;
        m_verified   = false;
        setOtaPhase(QStringLiteral("Error"), message, false);
        emit connectionStateChanged();
    }
}

void OtaController::onDeviceInfoSnapshot(const QByteArray &raw) {
    applyDeviceInfo(raw);
}

void OtaController::onControlFrameReceived(const OnyxOta::DecodedFrame &frame) {
    switch (frame.msgType) {
    case OnyxOta::RSP_DEVICE_INFO:
        applyDeviceInfo(frame.payload);
        break;
    case OnyxOta::RSP_ACK:
        handleAck(frame.payload);
        break;
    case OnyxOta::EVT_PROGRESS:
        handleProgress(frame.payload);
        break;
    case OnyxOta::RSP_ERROR:
        handleError(frame.payload);
        break;
    default:
        break;
    }
}

void OtaController::applyDeviceInfo(const QByteArray &raw) {
    if (raw.size() < static_cast<qsizetype>(sizeof(OnyxOta::DeviceInfoPayload))) return;

    OnyxOta::DeviceInfoPayload info{};
    std::memcpy(&info, raw.constData(), sizeof(info));

    // Strict brand handshake verification
    if (std::memcmp(info.magic, "ONYX", 4) != 0 || info.protocolVersion != OnyxOta::kProtocolVersion) {
        appendLog(QStringLiteral("Rejected non-Onyx device during protocol handshake."));
        teardownTransport();
        m_connecting = false;
        m_verified   = false;
        setOtaPhase(QStringLiteral("Error"), QStringLiteral("Device rejected: invalid Onyx signature."), false);
        emit connectionStateChanged();
        return;
    }

    const bool firstVerify = !m_verified;
    m_handshakeTimer.stop();
    m_connecting = false;
    m_verified   = true;

    m_speakerProductId = info.productId;
    m_speakerMajor     = info.fwMajor;
    m_speakerMinor     = info.fwMinor;
    m_speakerPatch     = info.fwPatch;
    m_speakerModel     = QString::fromLatin1(info.modelName, strnlen(info.modelName, sizeof(info.modelName)));
    m_speakerVersion   = QStringLiteral("v%1.%2.%3").arg(info.fwMajor).arg(info.fwMinor).arg(info.fwPatch);
    m_speakerBuildInfo = QStringLiteral("%1 %2")
                             .arg(QString::fromLatin1(info.buildDate, strnlen(info.buildDate, sizeof(info.buildDate))),
                                  QString::fromLatin1(info.buildTime, strnlen(info.buildTime, sizeof(info.buildTime))));
    m_runningSlot = (info.runningSlot == 0) ? QStringLiteral("ota_0")
                  : (info.runningSlot == 1) ? QStringLiteral("ota_1")
                                            : QStringLiteral("factory");
    m_maxOtaSize  = info.maxOtaSize;
    m_batteryMv   = info.batteryMv;
    m_batteryPct  = info.batteryPct;
    m_statusFlags = info.statusFlags;
    m_classicMac  = OnyxOta::formatMacAddress(info.classicMac);

    // Link speaker's Classic BD_ADDR back into the scanner entry so one-click Classic upgrade works!
    const QBluetoothAddress classicAddr(m_classicMac);
    if (!classicAddr.isNull()) {
        m_selectedSpeaker.classicAddress = classicAddr;
        m_selectedSpeaker.hasClassic     = true;
        m_scanner->linkClassicMacToDevice(m_selectedSpeaker.id, classicAddr);
    }

    if (firstVerify) {
        appendLog(QStringLiteral("Verified %1 (%2, slot %3, max %4 KB, bat %5%%)")
                      .arg(m_speakerModel, m_speakerVersion, m_runningSlot)
                      .arg(m_maxOtaSize / 1024)
                      .arg(m_batteryPct));
        setOtaPhase(QStringLiteral("Ready"),
                    QStringLiteral("Connected to %1 (%2) via %3.")
                        .arg(m_speakerModel, m_speakerVersion, activeTransportName()),
                    false);
    }

    emit connectionStateChanged();
    emit speakerInfoChanged();
    emit firmwareFileChanged();
    emit otaStateChanged();
}

void OtaController::handleAck(const QByteArray &payload) {
    if (payload.size() < static_cast<qsizetype>(sizeof(OnyxOta::AckPayload))) return;

    OnyxOta::AckPayload ack{};
    std::memcpy(&ack, payload.constData(), sizeof(ack));

    switch (ack.origCmd) {
    case OnyxOta::CMD_OTA_BEGIN: {
        m_watchdogTimer.stop();
        const quint16 transportMax = m_transport ? m_transport->maxDataChunk() : 240;
        m_chunkSize     = std::min<quint16>(ack.chunkSize > 0 ? ack.chunkSize : transportMax, transportMax);
        m_windowPackets = (ack.detail > 0 && ack.detail <= 12) ? static_cast<quint8>(ack.detail) : 6;
        m_nextOffset    = ack.nextOffset;
        m_ackedOffset   = ack.nextOffset;
        m_retryCount    = 0;

        setOtaPhase(QStringLiteral("Flashing"),
                    QStringLiteral("Streaming firmware (%1 B chunks, window %2)...")
                        .arg(m_chunkSize)
                        .arg(m_windowPackets),
                    true);
        appendLog(QStringLiteral("OTA slot ready: chunk=%1 B, window=%2 packets")
                      .arg(m_chunkSize)
                      .arg(m_windowPackets));
        sendNextWindow();
        break;
    }

    case OnyxOta::CMD_OTA_DATA:
        if (!m_otaInProgress) return;
        m_watchdogTimer.stop();
        m_blePaceTimer.stop();
        m_ackedOffset = ack.nextOffset;
        m_nextOffset  = ack.nextOffset;
        m_retryCount  = 0;
        updateTransferStats();

        if (m_ackedOffset >= m_fwInfo.sizeBytes) {
            setOtaPhase(QStringLiteral("Verifying"),
                        QStringLiteral("Verifying SHA-256 & Onyx firmware signature on speaker..."),
                        true);
            appendLog(QStringLiteral("All %1 bytes received. Committing & verifying SHA-256...").arg(m_ackedOffset));
            m_transport->sendControlFrame(OnyxOta::CMD_OTA_COMMIT);
            m_watchdogTimer.start(12000);
        } else {
            sendNextWindow();
        }
        break;

    case OnyxOta::CMD_OTA_COMMIT:
        m_watchdogTimer.stop();
        setOtaPhase(QStringLiteral("Rebooting"),
                    QStringLiteral("Verified! Boot slot switched — rebooting speaker..."),
                    true);
        appendLog(QStringLiteral("SHA-256 & Onyx descriptor verified! Requesting clean reboot..."));
        m_expectReboot = true;
        m_transport->sendControlFrame(OnyxOta::CMD_OTA_REBOOT);
        m_watchdogTimer.start(5000);
        break;

    case OnyxOta::CMD_OTA_REBOOT:
        m_watchdogTimer.stop();
        m_progress = 1.0;
        emit otaProgressChanged();
        setOtaPhase(QStringLiteral("Complete"),
                    QStringLiteral("Speaker rebooting into %1!").arg(m_fwInfo.versionString()),
                    false);
        appendLog(QStringLiteral("Reboot confirmed by speaker."));
        break;

    case OnyxOta::CMD_OTA_ABORT:
        m_watchdogTimer.stop();
        m_blePaceTimer.stop();
        setOtaPhase(QStringLiteral("Aborted"), QStringLiteral("Speaker confirmed OTA abort."), false);
        break;

    default:
        break;
    }
}

void OtaController::handleProgress(const QByteArray &payload) {
    if (payload.size() < static_cast<qsizetype>(sizeof(OnyxOta::ProgressPayload))) return;
    OnyxOta::ProgressPayload p{};
    std::memcpy(&p, payload.constData(), sizeof(p));
    if (p.totalBytes > 0) {
        m_ackedOffset = std::max(m_ackedOffset, p.bytesReceived);
        updateTransferStats();
    }
}

void OtaController::handleError(const QByteArray &payload) {
    if (payload.size() < static_cast<qsizetype>(sizeof(OnyxOta::ErrorPayload))) return;
    OnyxOta::ErrorPayload err{};
    std::memcpy(&err, payload.constData(), sizeof(err));
    const QString detail = QString::fromUtf8(payload.mid(sizeof(err)));

    // Recoverable BLE/RFCOMM packet drop: rewind cleanly to speaker's expected offset
    if (err.errCode == OnyxOta::ERR_OFFSET_MISMATCH && m_otaInProgress) {
        m_blePaceTimer.stop();
        m_watchdogTimer.stop();
        ++m_retryCount;
        if (m_retryCount <= 8) {
            appendLog(QStringLiteral("Resyncing transfer at offset %1 (attempt %2/8)...")
                          .arg(err.expectedOffset)
                          .arg(m_retryCount));
            m_ackedOffset = err.expectedOffset;
            m_nextOffset  = err.expectedOffset;
            sendNextWindow();
            return;
        }
    }

    const QString msg = detail.isEmpty()
                            ? OnyxOta::errorCodeToString(err.errCode)
                            : QStringLiteral("%1 (%2)").arg(OnyxOta::errorCodeToString(err.errCode), detail);
    finishOtaWithError(msg);
}

void OtaController::sendNextWindow() {
    if (!m_otaInProgress || !m_transport || !m_transport->isConnected()) return;

    if (m_transport->kind() == OtaTransport::Kind::Ble) {
        // Pace BLE WriteWithoutResponse chunks at 8 ms intervals so Android/BlueZ queues stay lossless
        m_windowLeft = m_windowPackets;
        m_blePaceTimer.start();
        onBlePaceTick();
        m_watchdogTimer.start(6000);
        return;
    }

    // Classic SPP RFCOMM: send the full window immediately into kernel RFCOMM buffer
    for (int i = 0; i < m_windowPackets && m_nextOffset < m_fwInfo.sizeBytes; ++i) {
        const quint32 remaining = m_fwInfo.sizeBytes - m_nextOffset;
        const quint32 len       = std::min<quint32>(remaining, m_chunkSize);
        const QByteArray chunk  = m_fwInfo.rawData.mid(static_cast<qsizetype>(m_nextOffset),
                                                       static_cast<qsizetype>(len));
        if (!m_transport->sendDataChunk(m_nextOffset, chunk)) {
            finishOtaWithError(QStringLiteral("Failed writing firmware chunk to Classic SPP socket."));
            return;
        }
        m_nextOffset += len;
    }
    m_watchdogTimer.start(6000);
}

void OtaController::onBlePaceTick() {
    if (!m_otaInProgress || !m_transport || m_windowLeft <= 0 || m_nextOffset >= m_fwInfo.sizeBytes) {
        m_blePaceTimer.stop();
        return;
    }

    const quint32 remaining = m_fwInfo.sizeBytes - m_nextOffset;
    const quint32 len       = std::min<quint32>(remaining, m_chunkSize);
    const QByteArray chunk  = m_fwInfo.rawData.mid(static_cast<qsizetype>(m_nextOffset),
                                                   static_cast<qsizetype>(len));
    if (!m_transport->sendDataChunk(m_nextOffset, chunk)) {
        m_blePaceTimer.stop();
        finishOtaWithError(QStringLiteral("Failed writing BLE GATT OTA_DATA characteristic."));
        return;
    }

    m_nextOffset += len;
    --m_windowLeft;
    if (m_windowLeft <= 0 || m_nextOffset >= m_fwInfo.sizeBytes) {
        m_blePaceTimer.stop();
    }
}

void OtaController::onWatchdogTimeout() {
    if (!m_otaInProgress) return;
    ++m_retryCount;
    if (m_otaPhase == QStringLiteral("Flashing") && m_retryCount <= 5) {
        appendLog(QStringLiteral("ACK timeout at offset %1 — retrying window (%2/5)...")
                      .arg(m_ackedOffset)
                      .arg(m_retryCount));
        m_nextOffset = m_ackedOffset;
        sendNextWindow();
        return;
    }
    finishOtaWithError(QStringLiteral("Speaker did not respond before timeout."));
}

void OtaController::updateTransferStats() {
    if (m_fwInfo.sizeBytes == 0) return;
    m_progress = std::clamp(static_cast<double>(m_ackedOffset) / static_cast<double>(m_fwInfo.sizeBytes), 0.0, 1.0);

    const qint64 elapsedMs = m_transferTimer.elapsed();
    if (elapsedMs > 200 && m_ackedOffset > 0) {
        const double bytesPerSec = (static_cast<double>(m_ackedOffset) * 1000.0) / static_cast<double>(elapsedMs);
        m_speedKBps = bytesPerSec / 1024.0;
        const quint32 rem = (m_fwInfo.sizeBytes > m_ackedOffset) ? (m_fwInfo.sizeBytes - m_ackedOffset) : 0;
        m_etaSeconds = (bytesPerSec > 1.0) ? static_cast<int>(rem / bytesPerSec) : 0;
    }
    emit otaProgressChanged();
}

void OtaController::finishOtaWithError(const QString &msg) {
    m_blePaceTimer.stop();
    m_watchdogTimer.stop();
    appendLog(QStringLiteral("OTA Error: %1").arg(msg));
    setOtaPhase(QStringLiteral("Error"), msg, false);
}

void OtaController::setOtaPhase(const QString &phase, const QString &status, bool inProgress) {
    m_otaPhase      = phase;
    m_otaStatusText = status;
    m_otaInProgress = inProgress;
    emit otaStateChanged();
    emit speakerInfoChanged();
}

void OtaController::appendLog(const QString &line) {
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_logs.append(QStringLiteral("[%1] %2").arg(ts, line));
    if (m_logs.size() > 150) {
        m_logs.removeFirst();
    }
    emit logsChanged();
}
