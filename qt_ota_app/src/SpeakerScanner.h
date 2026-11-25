#pragma once

#include "SpeakerDevice.h"

#include <QAbstractListModel>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothLocalDevice>
#include <QBluetoothServiceDiscoveryAgent>
#include <QList>
#include <QSet>

class SpeakerScanner : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        DisplayAddressRole,
        HasBleRole,
        HasClassicRole,
        PairedClassicRole,
        AdvertisedVersionRole,
        RssiRole,
    };

    explicit SpeakerScanner(QObject *parent = nullptr);
    ~SpeakerScanner() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isScanning() const { return m_scanning; }
    QString statusMessage() const { return m_statusMessage; }

    Q_INVOKABLE void startScan();
    Q_INVOKABLE void stopScan();

    const SpeakerDevice *deviceAt(int index) const;
    const SpeakerDevice *findById(const QString &id) const;
    void linkClassicMacToDevice(const QString &deviceId, const QBluetoothAddress &classicAddr);

signals:
    void scanningChanged();
    void statusMessageChanged();
    void countChanged();
    void scanError(const QString &message);

private slots:
    void onDeviceDiscovered(const QBluetoothDeviceInfo &info);
    void onDeviceUpdated(const QBluetoothDeviceInfo &info, QBluetoothDeviceInfo::Fields updatedFields);
    void onDiscoveryFinished();
    void onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error);

    void onSdpServiceDiscovered(const QBluetoothServiceInfo &service);
    void onSdpFinished();

private:
    bool matchesOnyxBle(const QBluetoothDeviceInfo &info,
                        quint8 &modelLo, quint8 &maj, quint8 &min, quint8 &pat, bool &hasVer) const;
    bool matchesOnyxClassic(const QBluetoothDeviceInfo &info,
                            quint8 &modelLo, quint8 &maj, quint8 &min, quint8 &pat, bool &hasVer) const;
    static bool areLikelySameEsp32(const QBluetoothAddress &a, const QBluetoothAddress &b);
    int  findMatchingSpeakerIndex(const QBluetoothAddress &addr, const QString &name) const;
    void upsertSpeaker(const SpeakerDevice &candidate);
    void probeConnectedClassicDevices();
    void startNextSdpProbe();
    bool ensureBluetoothReady();
    void setStatus(const QString &msg);

    QBluetoothDeviceDiscoveryAgent  *m_agent      = nullptr;
    QBluetoothLocalDevice           *m_localDev   = nullptr;
    QBluetoothServiceDiscoveryAgent *m_sdpAgent   = nullptr;
    QList<SpeakerDevice>             m_devices;
    QList<QBluetoothAddress>         m_sdpQueue;
    QSet<QString>                    m_probedSdpAddrs;
    bool                             m_scanning   = false;
    QString                          m_statusMessage;
};
