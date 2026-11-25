#pragma once

#include "OtaTransport.h"

#include <QBluetoothSocket>

class ClassicOtaTransport : public OtaTransport {
    Q_OBJECT
public:
    explicit ClassicOtaTransport(QObject *parent = nullptr);
    ~ClassicOtaTransport() override;

    Kind    kind() const override { return Kind::ClassicSpp; }
    QString kindName() const override { return QStringLiteral("Classic SPP"); }
    bool    isConnected() const override { return m_connected; }
    quint16 maxDataChunk() const override { return 512; }

    void connectToSpeaker(const SpeakerDevice &speaker) override;
    void disconnectFromSpeaker() override;
    bool sendControlFrame(quint8 msgType, const QByteArray &payload = {}, quint8 flags = 0) override;
    bool sendDataChunk(quint32 offset, const QByteArray &chunk) override;

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketReadyRead();
    void onSocketError(QBluetoothSocket::SocketError error);

private:
    void cleanup();

    QBluetoothSocket           *m_socket    = nullptr;
    bool                        m_connected = false;
    OnyxOta::FrameStreamParser  m_parser;
};
