#include "ClassicOtaTransport.h"

#include <QBluetoothUuid>

ClassicOtaTransport::ClassicOtaTransport(QObject *parent)
    : OtaTransport(parent)
{
}

ClassicOtaTransport::~ClassicOtaTransport() {
    cleanup();
}

void ClassicOtaTransport::connectToSpeaker(const SpeakerDevice &speaker) {
    cleanup();
    if (!speaker.hasClassic || speaker.classicAddress.isNull()) {
        emit errorOccurred(QStringLiteral("Speaker Classic Bluetooth address is not available."));
        return;
    }

    m_socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
    connect(m_socket, &QBluetoothSocket::connected,
            this, &ClassicOtaTransport::onSocketConnected);
    connect(m_socket, &QBluetoothSocket::disconnected,
            this, &ClassicOtaTransport::onSocketDisconnected);
    connect(m_socket, &QBluetoothSocket::readyRead,
            this, &ClassicOtaTransport::onSocketReadyRead);
    connect(m_socket, &QBluetoothSocket::errorOccurred,
            this, &ClassicOtaTransport::onSocketError);

    m_parser.reset();
    m_socket->connectToService(
        speaker.classicAddress,
        QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));
}

void ClassicOtaTransport::disconnectFromSpeaker() {
    if (m_socket && m_socket->state() != QBluetoothSocket::SocketState::UnconnectedState) {
        m_socket->disconnectFromService();
    } else {
        cleanup();
    }
}

bool ClassicOtaTransport::sendControlFrame(quint8 msgType, const QByteArray &payload, quint8 flags) {
    if (!m_connected || !m_socket) return false;
    const QByteArray frame = OnyxOta::encodeFrame(msgType, payload, flags);
    return m_socket->write(frame) == frame.size();
}

bool ClassicOtaTransport::sendDataChunk(quint32 offset, const QByteArray &chunk) {
    if (!m_connected || !m_socket) return false;
    QByteArray payload;
    payload.reserve(4 + chunk.size());
    payload.append(static_cast<char>(offset & 0xFF));
    payload.append(static_cast<char>((offset >> 8) & 0xFF));
    payload.append(static_cast<char>((offset >> 16) & 0xFF));
    payload.append(static_cast<char>((offset >> 24) & 0xFF));
    payload.append(chunk);
    return sendControlFrame(OnyxOta::CMD_OTA_DATA, payload);
}

void ClassicOtaTransport::onSocketConnected() {
    m_connected = true;
    emit transportReady();
}

void ClassicOtaTransport::onSocketDisconnected() {
    const bool wasConnected = m_connected;
    cleanup();
    if (wasConnected) {
        emit disconnected();
    }
}

void ClassicOtaTransport::onSocketReadyRead() {
    if (!m_socket) return;
    const QByteArray data = m_socket->readAll();
    const auto frames = m_parser.feed(data);
    for (const auto &f : frames) {
        emit controlFrameReceived(f);
    }
}

void ClassicOtaTransport::onSocketError(QBluetoothSocket::SocketError error) {
    Q_UNUSED(error);
    const QString msg = m_socket ? m_socket->errorString() : QStringLiteral("RFCOMM socket error");
    cleanup();
    emit errorOccurred(QStringLiteral("Classic SPP error: %1").arg(msg));
}

void ClassicOtaTransport::cleanup() {
    m_connected = false;
    m_parser.reset();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}
