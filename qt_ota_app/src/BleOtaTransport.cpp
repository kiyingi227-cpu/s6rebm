#include "BleOtaTransport.h"

#include <QBluetoothUuid>
#include <QLowEnergyDescriptor>
#include <algorithm>

BleOtaTransport::BleOtaTransport(QObject *parent)
    : OtaTransport(parent)
{
}

BleOtaTransport::~BleOtaTransport() {
    cleanup();
}

quint16 BleOtaTransport::maxDataChunk() const {
    const int effectiveMtu = (m_ctrl && m_ctrl->mtu() > m_mtu) ? m_ctrl->mtu() : m_mtu;
    int usable = (effectiveMtu > 23) ? (effectiveMtu - 7) : 16;
    usable = std::clamp(usable, 16, 496);
    usable &= ~3;
    return static_cast<quint16>(std::max(usable, 16));
}

void BleOtaTransport::connectToSpeaker(const SpeakerDevice &speaker) {
    cleanup();
    if (!speaker.hasBle) {
        emit errorOccurred(QStringLiteral("Speaker does not have BLE discovery info."));
        return;
    }

    m_ctrl = QLowEnergyController::createCentral(speaker.bleInfo, this);
    m_ctrl->setRemoteAddressType(QLowEnergyController::PublicAddress);

    connect(m_ctrl, &QLowEnergyController::connected,
            this, &BleOtaTransport::onControllerConnected);
    connect(m_ctrl, &QLowEnergyController::disconnected,
            this, &BleOtaTransport::onControllerDisconnected);
    connect(m_ctrl, &QLowEnergyController::errorOccurred,
            this, &BleOtaTransport::onControllerError);
    connect(m_ctrl, &QLowEnergyController::serviceDiscovered,
            this, &BleOtaTransport::onServiceDiscovered);
    connect(m_ctrl, &QLowEnergyController::discoveryFinished,
            this, &BleOtaTransport::onServiceScanDone);
    connect(m_ctrl, &QLowEnergyController::mtuChanged,
            this, &BleOtaTransport::onMtuChanged);

    m_ctrl->connectToDevice();
}

void BleOtaTransport::disconnectFromSpeaker() {
    if (m_ctrl && m_ctrl->state() != QLowEnergyController::UnconnectedState) {
        m_ctrl->disconnectFromDevice();
    } else {
        cleanup();
    }
}

bool BleOtaTransport::sendControlFrame(quint8 msgType, const QByteArray &payload, quint8 flags) {
    if (!m_ready || !m_service || !m_charCtrl.isValid()) return false;
    const QByteArray frame = OnyxOta::encodeFrame(msgType, payload, flags);
    m_service->writeCharacteristic(m_charCtrl, frame, QLowEnergyService::WriteWithResponse);
    return true;
}

bool BleOtaTransport::sendDataChunk(quint32 offset, const QByteArray &chunk) {
    if (!m_ready || !m_service || !m_charData.isValid()) return false;
    QByteArray pkt;
    pkt.reserve(4 + chunk.size());
    pkt.append(static_cast<char>(offset & 0xFF));
    pkt.append(static_cast<char>((offset >> 8) & 0xFF));
    pkt.append(static_cast<char>((offset >> 16) & 0xFF));
    pkt.append(static_cast<char>((offset >> 24) & 0xFF));
    pkt.append(chunk);

    const auto mode = m_charData.properties().testFlag(QLowEnergyCharacteristic::WriteNoResponse)
                          ? QLowEnergyService::WriteWithoutResponse
                          : QLowEnergyService::WriteWithResponse;
    m_service->writeCharacteristic(m_charData, pkt, mode);
    return true;
}

void BleOtaTransport::onControllerConnected() {
    m_mtu = m_ctrl->mtu();
    m_hasOtaService = false;
    m_ctrl->discoverServices();
}

void BleOtaTransport::onControllerDisconnected() {
    const bool wasReady = m_ready;
    cleanup();
    if (wasReady) {
        emit disconnected();
    }
}

void BleOtaTransport::onControllerError(QLowEnergyController::Error error) {
    Q_UNUSED(error);
    const QString msg = m_ctrl ? m_ctrl->errorString() : QStringLiteral("BLE controller error");
    cleanup();
    emit errorOccurred(QStringLiteral("BLE error: %1").arg(msg));
}

void BleOtaTransport::onServiceDiscovered(const QBluetoothUuid &uuid) {
    if (uuid == OnyxOta::kServiceUuid) {
        m_hasOtaService = true;
    }
}

void BleOtaTransport::onServiceScanDone() {
    if (!m_ctrl) return;
    if (!m_hasOtaService) {
        emit errorOccurred(QStringLiteral("Device does not expose the Onyx Studio OTA GATT service."));
        disconnectFromSpeaker();
        return;
    }

    m_service = m_ctrl->createServiceObject(OnyxOta::kServiceUuid, this);
    if (!m_service) {
        emit errorOccurred(QStringLiteral("Failed to create Onyx OTA GATT service object."));
        disconnectFromSpeaker();
        return;
    }

    connect(m_service, &QLowEnergyService::stateChanged,
            this, &BleOtaTransport::onServiceStateChanged);
    connect(m_service, &QLowEnergyService::characteristicChanged,
            this, &BleOtaTransport::onCharacteristicChanged);
    connect(m_service, &QLowEnergyService::characteristicRead,
            this, &BleOtaTransport::onCharacteristicRead);
    connect(m_service, &QLowEnergyService::descriptorWritten,
            this, &BleOtaTransport::onDescriptorWritten);
    connect(m_service, &QLowEnergyService::errorOccurred,
            this, &BleOtaTransport::onServiceError);

    m_service->discoverDetails();
}

void BleOtaTransport::onMtuChanged(int mtu) {
    if (mtu > 0) m_mtu = mtu;
}

void BleOtaTransport::onServiceStateChanged(QLowEnergyService::ServiceState newState) {
    if (newState != QLowEnergyService::RemoteServiceDiscovered || !m_service) return;

    m_charInfo = m_service->characteristic(OnyxOta::kCharInfoUuid);
    m_charCtrl = m_service->characteristic(OnyxOta::kCharCtrlUuid);
    m_charData = m_service->characteristic(OnyxOta::kCharDataUuid);

    if (!m_charInfo.isValid() || !m_charCtrl.isValid() || !m_charData.isValid()) {
        emit errorOccurred(QStringLiteral("Onyx OTA GATT service is missing required characteristics."));
        disconnectFromSpeaker();
        return;
    }

    const QByteArray enableNotify = QByteArray::fromHex("0100");
    const auto ctrlCccd = m_charCtrl.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    const auto infoCccd = m_charInfo.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);

    if (infoCccd.isValid()) {
        m_service->writeDescriptor(infoCccd, enableNotify);
    }
    if (ctrlCccd.isValid()) {
        m_service->writeDescriptor(ctrlCccd, enableNotify);
    } else {
        m_ctrlNotifyOn = true;
        m_ready        = true;
        m_service->readCharacteristic(m_charInfo);
        emit transportReady();
    }
}

void BleOtaTransport::onDescriptorWritten(const QLowEnergyDescriptor &d, const QByteArray &value) {
    if (!m_service) return;
    const auto ctrlCccd = m_charCtrl.descriptor(
        QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (d == ctrlCccd && value == QByteArray::fromHex("0100") && !m_ready) {
        m_ctrlNotifyOn = true;
        m_ready        = true;
        m_service->readCharacteristic(m_charInfo);
        emit transportReady();
    }
}

void BleOtaTransport::onCharacteristicRead(const QLowEnergyCharacteristic &c, const QByteArray &value) {
    if (c.uuid() == OnyxOta::kCharInfoUuid && !value.isEmpty()) {
        emit deviceInfoSnapshot(value);
    }
}

void BleOtaTransport::onCharacteristicChanged(const QLowEnergyCharacteristic &c, const QByteArray &value) {
    if (c.uuid() == OnyxOta::kCharInfoUuid) {
        emit deviceInfoSnapshot(value);
    } else if (c.uuid() == OnyxOta::kCharCtrlUuid) {
        OnyxOta::DecodedFrame frame;
        if (OnyxOta::decodeSingleFrame(value, frame)) {
            emit controlFrameReceived(frame);
        }
    }
}

void BleOtaTransport::onServiceError(QLowEnergyService::ServiceError error) {
    if (error == QLowEnergyService::NoError) return;
    emit errorOccurred(QStringLiteral("BLE GATT service error (%1)").arg(static_cast<int>(error)));
}

void BleOtaTransport::cleanup() {
    m_ready         = false;
    m_ctrlNotifyOn  = false;
    m_hasOtaService = false;
    if (m_service) {
        m_service->disconnect(this);
        m_service->deleteLater();
        m_service = nullptr;
    }
    if (m_ctrl) {
        m_ctrl->disconnect(this);
        m_ctrl->deleteLater();
        m_ctrl = nullptr;
    }
    m_charInfo = {};
    m_charCtrl = {};
    m_charData = {};
}
