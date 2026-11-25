#pragma once

#include "OtaProtocol.h"
#include "OtaTransport.h"
#include "SpeakerScanner.h"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>

class OtaController : public QObject {
    Q_OBJECT

    // Scanner & permissions
    Q_PROPERTY(SpeakerScanner* scanner READ scanner CONSTANT)
    Q_PROPERTY(bool bluetoothPermissionGranted READ bluetoothPermissionGranted NOTIFY permissionChanged)
    Q_PROPERTY(QString permissionStatus READ permissionStatus NOTIFY permissionChanged)

    // Connection & transport
    Q_PROPERTY(int transportPreference READ transportPreference WRITE setTransportPreference NOTIFY transportPreferenceChanged)
    Q_PROPERTY(bool connecting READ isConnecting NOTIFY connectionStateChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(QString activeTransportName READ activeTransportName NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectedDeviceId READ connectedDeviceId NOTIFY connectionStateChanged)
    Q_PROPERTY(bool canSwitchToClassic READ canSwitchToClassic NOTIFY speakerInfoChanged)
    Q_PROPERTY(bool canSwitchToBle READ canSwitchToBle NOTIFY speakerInfoChanged)

    // Speaker live telemetry
    Q_PROPERTY(QString speakerModel READ speakerModel NOTIFY speakerInfoChanged)
    Q_PROPERTY(QString speakerVersion READ speakerVersion NOTIFY speakerInfoChanged)
    Q_PROPERTY(QString speakerBuildInfo READ speakerBuildInfo NOTIFY speakerInfoChanged)
    Q_PROPERTY(QString runningSlot READ runningSlot NOTIFY speakerInfoChanged)
    Q_PROPERTY(quint32 maxOtaSize READ maxOtaSize NOTIFY speakerInfoChanged)
    Q_PROPERTY(double batteryVolts READ batteryVolts NOTIFY speakerInfoChanged)
    Q_PROPERTY(int batteryPercent READ batteryPercent NOTIFY speakerInfoChanged)
    Q_PROPERTY(bool dcPresent READ dcPresent NOTIFY speakerInfoChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY speakerInfoChanged)
    Q_PROPERTY(bool lowBattery READ lowBattery NOTIFY speakerInfoChanged)
    Q_PROPERTY(bool btAudioConnected READ btAudioConnected NOTIFY speakerInfoChanged)
    Q_PROPERTY(QString classicMac READ classicMac NOTIFY speakerInfoChanged)

    // Selected firmware file
    Q_PROPERTY(bool fileLoaded READ fileLoaded NOTIFY firmwareFileChanged)
    Q_PROPERTY(bool fileValid READ fileValid NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileModelName READ fileModelName NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileVersion READ fileVersion NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileBuildInfo READ fileBuildInfo NOTIFY firmwareFileChanged)
    Q_PROPERTY(quint32 fileSizeBytes READ fileSizeBytes NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileSha256Short READ fileSha256Short NOTIFY firmwareFileChanged)
    Q_PROPERTY(QString fileCompatibilityText READ fileCompatibilityText NOTIFY firmwareFileChanged)
    Q_PROPERTY(bool fileCompatibleWithSpeaker READ fileCompatibleWithSpeaker NOTIFY firmwareFileChanged)

    // OTA transfer state & progress
    Q_PROPERTY(QString otaPhase READ otaPhase NOTIFY otaStateChanged)
    Q_PROPERTY(QString otaStatusText READ otaStatusText NOTIFY otaStateChanged)
    Q_PROPERTY(bool otaInProgress READ otaInProgress NOTIFY otaStateChanged)
    Q_PROPERTY(bool canStartOta READ canStartOta NOTIFY otaStateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY otaProgressChanged)
    Q_PROPERTY(quint32 bytesSent READ bytesSent NOTIFY otaProgressChanged)
    Q_PROPERTY(quint32 totalBytes READ totalBytes NOTIFY otaProgressChanged)
    Q_PROPERTY(double speedKBps READ speedKBps NOTIFY otaProgressChanged)
    Q_PROPERTY(int etaSeconds READ etaSeconds NOTIFY otaProgressChanged)
    Q_PROPERTY(QStringList logs READ logs NOTIFY logsChanged)

public:
    enum TransportPref {
        PreferAuto    = 0,
        PreferClassic = 1,
        PreferBle     = 2,
    };
    Q_ENUM(TransportPref)

    explicit OtaController(QObject *parent = nullptr);
    ~OtaController() override;

    SpeakerScanner *scanner() const { return m_scanner; }
    bool bluetoothPermissionGranted() const { return m_btPermGranted; }
    QString permissionStatus() const { return m_permStatus; }

    int transportPreference() const { return m_transportPref; }
    void setTransportPreference(int pref);

    bool isConnecting() const { return m_connecting; }
    bool isConnected() const { return m_verified; }
    QString activeTransportName() const;
    QString connectedDeviceId() const { return m_selectedSpeaker.id; }
    bool canSwitchToClassic() const;
    bool canSwitchToBle() const;

    QString speakerModel() const { return m_speakerModel; }
    QString speakerVersion() const { return m_speakerVersion; }
    QString speakerBuildInfo() const { return m_speakerBuildInfo; }
    QString runningSlot() const { return m_runningSlot; }
    quint32 maxOtaSize() const { return m_maxOtaSize; }
    double  batteryVolts() const { return m_batteryMv / 1000.0; }
    int     batteryPercent() const { return m_batteryPct; }
    bool    dcPresent() const { return (m_statusFlags & OnyxOta::STATUS_DC_PRESENT) != 0; }
    bool    charging() const { return (m_statusFlags & OnyxOta::STATUS_CHARGING) != 0; }
    bool    lowBattery() const { return (m_statusFlags & OnyxOta::STATUS_LOW_BATTERY) != 0; }
    bool    btAudioConnected() const { return (m_statusFlags & OnyxOta::STATUS_BT_AUDIO_CONN) != 0; }
    QString classicMac() const { return m_classicMac; }

    bool    fileLoaded() const { return m_fileLoaded; }
    bool    fileValid() const { return m_fwInfo.valid; }
    QString fileName() const { return m_fwInfo.fileName; }
    QString fileModelName() const { return m_fwInfo.modelName; }
    QString fileVersion() const { return m_fwInfo.valid ? m_fwInfo.versionString() : QString(); }
    QString fileBuildInfo() const;
    quint32 fileSizeBytes() const { return m_fwInfo.sizeBytes; }
    QString fileSha256Short() const;
    QString fileCompatibilityText() const;
    bool    fileCompatibleWithSpeaker() const;

    QString otaPhase() const { return m_otaPhase; }
    QString otaStatusText() const { return m_otaStatusText; }
    bool    otaInProgress() const { return m_otaInProgress; }
    bool    canStartOta() const;
    double  progress() const { return m_progress; }
    quint32 bytesSent() const { return m_ackedOffset; }
    quint32 totalBytes() const { return m_fwInfo.sizeBytes; }
    double  speedKBps() const { return m_speedKBps; }
    int     etaSeconds() const { return m_etaSeconds; }
    QStringList logs() const { return m_logs; }

    Q_INVOKABLE void requestBluetoothPermissions();
    Q_INVOKABLE void startScan();
    Q_INVOKABLE void connectToDevice(const QString &deviceId);
    Q_INVOKABLE void switchTransport(int targetKind); // 1 = Classic, 2 = BLE
    Q_INVOKABLE void disconnectSpeaker();
    Q_INVOKABLE void refreshSpeakerInfo();
    Q_INVOKABLE void loadFirmwareFile(const QUrl &fileUrl);
    Q_INVOKABLE void startOta();
    Q_INVOKABLE void abortOta();
    Q_INVOKABLE void clearLogs();

signals:
    void permissionChanged();
    void transportPreferenceChanged();
    void connectionStateChanged();
    void speakerInfoChanged();
    void firmwareFileChanged();
    void otaStateChanged();
    void otaProgressChanged();
    void logsChanged();

private slots:
    void onTransportReady();
    void onTransportDisconnected();
    void onTransportError(const QString &message);
    void onControlFrameReceived(const OnyxOta::DecodedFrame &frame);
    void onDeviceInfoSnapshot(const QByteArray &raw);
    void onBlePaceTick();
    void onWatchdogTimeout();
    void onHandshakeTimeout();

private:
    void openTransport(OtaTransport::Kind kind);
    void teardownTransport();
    void applyDeviceInfo(const QByteArray &raw);
    void handleAck(const QByteArray &payload);
    void handleProgress(const QByteArray &payload);
    void handleError(const QByteArray &payload);

    void sendNextWindow();
    void updateTransferStats();
    void finishOtaWithError(const QString &msg);
    void setOtaPhase(const QString &phase, const QString &status, bool inProgress);
    void appendLog(const QString &line);

    SpeakerScanner *m_scanner          = nullptr;
    OtaTransport   *m_transport        = nullptr;
    SpeakerDevice   m_selectedSpeaker;

    bool    m_btPermGranted = true;
    QString m_permStatus    = QStringLiteral("Bluetooth permissions ready");

    int     m_transportPref = PreferAuto;
    bool    m_connecting    = false;
    bool    m_verified      = false;

    // Live speaker info
    quint16 m_speakerProductId = 0;
    quint8  m_speakerMajor     = 0;
    quint8  m_speakerMinor     = 0;
    quint8  m_speakerPatch     = 0;
    QString m_speakerModel;
    QString m_speakerVersion;
    QString m_speakerBuildInfo;
    QString m_runningSlot;
    quint32 m_maxOtaSize       = 0;
    quint16 m_batteryMv        = 0;
    quint8  m_batteryPct       = 0;
    quint16 m_statusFlags      = 0;
    QString m_classicMac;

    // Firmware file info
    bool                       m_fileLoaded = false;
    OnyxOta::FirmwareImageInfo m_fwInfo;

    // OTA transfer runtime
    QString       m_otaPhase      = QStringLiteral("Idle");
    QString       m_otaStatusText = QStringLiteral("Connect a speaker and select a firmware .bin file.");
    bool          m_otaInProgress = false;
    bool          m_expectReboot  = false;
    quint16       m_chunkSize     = 240;
    quint8        m_windowPackets = 6;
    quint32       m_nextOffset    = 0;
    quint32       m_ackedOffset   = 0;
    int           m_windowLeft    = 0;
    int           m_retryCount    = 0;
    double        m_progress      = 0.0;
    double        m_speedKBps     = 0.0;
    int           m_etaSeconds    = 0;
    QElapsedTimer m_transferTimer;
    QTimer        m_blePaceTimer;
    QTimer        m_watchdogTimer;
    QTimer        m_handshakeTimer;
    QStringList   m_logs;
};
