#pragma once

#include "OtaProtocol.h"
#include "SpeakerDevice.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class OtaTransport : public QObject {
    Q_OBJECT
public:
    enum class Kind {
        Ble,
        ClassicSpp,
    };

    explicit OtaTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~OtaTransport() override = default;

    virtual Kind    kind() const = 0;
    virtual QString kindName() const = 0;
    virtual bool    isConnected() const = 0;
    virtual quint16 maxDataChunk() const = 0;

    virtual void connectToSpeaker(const SpeakerDevice &speaker) = 0;
    virtual void disconnectFromSpeaker() = 0;
    virtual bool sendControlFrame(quint8 msgType, const QByteArray &payload = {}, quint8 flags = 0) = 0;
    virtual bool sendDataChunk(quint32 offset, const QByteArray &chunk) = 0;

signals:
    void transportReady();
    void disconnected();
    void controlFrameReceived(const OnyxOta::DecodedFrame &frame);
    void deviceInfoSnapshot(const QByteArray &rawDeviceInfo);
    void errorOccurred(const QString &message);
};
