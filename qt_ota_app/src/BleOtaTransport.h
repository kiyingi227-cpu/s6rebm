#pragma once

#include "OtaTransport.h"

#include <QLowEnergyCharacteristic>
#include <QLowEnergyController>
#include <QLowEnergyService>

class BleOtaTransport : public OtaTransport {
    Q_OBJECT
public:
    explicit BleOtaTransport(QObject *parent = nullptr);
    ~BleOtaTransport() override;

    Kind    kind() const override { return Kind::Ble; }
    QString kindName() const override { return QStringLiteral("BLE GATT"); }
    bool    isConnected() const override { return m_ready; }
    quint16 maxDataChunk() const override;

    void connectToSpeaker(const SpeakerDevice &speaker) override;
    void disconnectFromSpeaker() override;
    bool sendControlFrame(quint8 msgType, const QByteArray &payload = {}, quint8 flags = 0) override;
    bool sendDataChunk(quint32 offset, const QByteArray &chunk) override;

private slots:
    void onControllerConnected();
    void onControllerDisconnected();
    void onControllerError(QLowEnergyController::Error error);
    void onServiceDiscovered(const QBluetoothUuid &uuid);
    void onServiceScanDone();
    void onMtuChanged(int mtu);

    void onServiceStateChanged(QLowEnergyService::ServiceState newState);
    void onCharacteristicChanged(const QLowEnergyCharacteristic &c, const QByteArray &value);
    void onCharacteristicRead(const QLowEnergyCharacteristic &c, const QByteArray &value);
    void onDescriptorWritten(const QLowEnergyDescriptor &d, const QByteArray &value);
    void onServiceError(QLowEnergyService::ServiceError error);

private:
    void cleanup();

    QLowEnergyController     *m_ctrl          = nullptr;
    QLowEnergyService        *m_service       = nullptr;
    QLowEnergyCharacteristic  m_charInfo;
    QLowEnergyCharacteristic  m_charCtrl;
    QLowEnergyCharacteristic  m_charData;
    bool                      m_hasOtaService = false;
    bool                      m_ctrlNotifyOn  = false;
    bool                      m_ready         = false;
    int                       m_mtu           = 23;
};
