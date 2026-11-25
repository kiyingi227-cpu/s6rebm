#include "SpeakerScanner.h"
#include "OtaProtocol.h"

#include <QBluetoothServiceInfo>
#include <QBluetoothUuid>
#include <cstdlib>

SpeakerScanner::SpeakerScanner(QObject *parent)
    : QAbstractListModel(parent)
{
    // Do NOT construct QBluetoothLocalDevice here: on Android 12+, constructing it
    // before BLUETOOTH_CONNECT permission is granted permanently marks it invalid.
    setStatus(QStringLiteral("Ready — tap Scan to discover Onyx Studio speakers"));
}

SpeakerScanner::~SpeakerScanner() {
    stopScan();
}

bool SpeakerScanner::ensureBluetoothReady() {
    // Re-create QBluetoothLocalDevice if it was not yet created or became invalid before permission grant
    if (!m_localDev || !m_localDev->isValid()) {
        if (m_localDev) {
            m_localDev->deleteLater();
            m_localDev = nullptr;
        }
        m_localDev = new QBluetoothLocalDevice(this);
    }

    if (!m_agent) {
        m_agent = new QBluetoothDeviceDiscoveryAgent(this);
        m_agent->setLowEnergyDiscoveryTimeout(12000);

        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
                this, &SpeakerScanner::onDeviceDiscovered);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::deviceUpdated,
                this, &SpeakerScanner::onDeviceUpdated);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::finished,
                this, &SpeakerScanner::onDiscoveryFinished);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::canceled,
                this, &SpeakerScanner::onDiscoveryFinished);
        connect(m_agent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
                this, &SpeakerScanner::onDiscoveryError);
    }

    if (!m_localDev->isValid()) {
        setStatus(QStringLiteral("Bluetooth adapter unavailable — grant Nearby Devices permission and try again"));
        return false;
    }
    if (m_localDev->hostMode() == QBluetoothLocalDevice::HostPoweredOff) {
        setStatus(QStringLiteral("Bluetooth is turned off — enable Bluetooth and tap Scan"));
        return false;
    }
    return true;
}

int SpeakerScanner::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return m_devices.size();
}

QVariant SpeakerScanner::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size())
        return {};

    const SpeakerDevice &d = m_devices.at(index.row());
    switch (role) {
    case IdRole:                return d.id;
    case NameRole:              return d.name;
    case DisplayAddressRole:    return d.displayAddress();
    case HasBleRole:            return d.hasBle;
    case HasClassicRole:        return d.hasClassic;
    case PairedClassicRole:     return d.pairedClassic;
    case AdvertisedVersionRole: return d.advertisedVersion();
    case RssiRole:              return d.rssi;
    default:                    return {};
    }
}

QHash<int, QByteArray> SpeakerScanner::roleNames() const {
    return {
        {IdRole,                "deviceId"},
        {NameRole,              "name"},
        {DisplayAddressRole,    "displayAddress"},
        {HasBleRole,            "hasBle"},
        {HasClassicRole,        "hasClassic"},
        {PairedClassicRole,     "pairedClassic"},
        {AdvertisedVersionRole, "advertisedVersion"},
        {RssiRole,              "rssi"},
    };
}

void SpeakerScanner::startScan() {
    if (m_scanning) stopScan();

    if (!ensureBluetoothReady()) {
        emit scanError(m_statusMessage);
        return;
    }

    beginResetModel();
    m_devices.clear();
    endResetModel();
    emit countChanged();

    m_probedSdpAddrs.clear();
    m_sdpQueue.clear();

    m_scanning = true;
    emit scanningChanged();
    setStatus(QStringLiteral("Scanning Classic & BLE for Onyx Studio speakers..."));

    // 1. Probe already-connected Classic devices for the ONYX_S6_OTA SPP service
    //    (handles the common case where A2DP audio is connected and Classic inquiry is hidden).
    probeConnectedClassicDevices();

    // 2. Run dual-mode discovery (BLE + Classic) with strict Onyx brand filtering.
    m_agent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod |
                   QBluetoothDeviceDiscoveryAgent::ClassicMethod);
}

void SpeakerScanner::stopScan() {
    if (m_agent && m_agent->isActive()) {
        m_agent->stop();
    }
    if (m_sdpAgent) {
        m_sdpAgent->stop();
        m_sdpAgent->deleteLater();
        m_sdpAgent = nullptr;
    }
    m_sdpQueue.clear();

    if (m_scanning) {
        m_scanning = false;
        emit scanningChanged();
    }
}

const SpeakerDevice *SpeakerScanner::deviceAt(int index) const {
    if (index < 0 || index >= m_devices.size()) return nullptr;
    return &m_devices.at(index);
}

const SpeakerDevice *SpeakerScanner::findById(const QString &id) const {
    for (const auto &d : m_devices) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

void SpeakerScanner::linkClassicMacToDevice(const QString &deviceId, const QBluetoothAddress &classicAddr) {
    if (classicAddr.isNull()) return;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].id == deviceId) {
            m_devices[i].classicAddress = classicAddr;
            m_devices[i].hasClassic     = true;
            m_devices[i].pairedClassic  =
                (m_localDev && m_localDev->isValid() &&
                 m_localDev->pairingStatus(classicAddr) != QBluetoothLocalDevice::Unpaired);
            const QModelIndex idx = index(i);
            emit dataChanged(idx, idx);
            return;
        }
    }
}

bool SpeakerScanner::matchesOnyxBle(const QBluetoothDeviceInfo &info,
                                    quint8 &modelLo, quint8 &maj, quint8 &min, quint8 &pat,
                                    bool &hasVer) const
{
    hasVer = false;

    // 1. Check Manufacturer Specific Data (Company 0x594F + magic "OS")
    const QByteArray mfg = info.manufacturerData(OnyxOta::kManufacturerCompanyId);
    bool mfgMatch = false;
    if (mfg.size() >= 6 &&
        static_cast<quint8>(mfg[0]) == OnyxOta::kMfgMagic0 &&
        static_cast<quint8>(mfg[1]) == OnyxOta::kMfgMagic1)
    {
        mfgMatch = true;
        modelLo  = static_cast<quint8>(mfg[2]);
        maj      = static_cast<quint8>(mfg[3]);
        min      = static_cast<quint8>(mfg[4]);
        pat      = static_cast<quint8>(mfg[5]);
        hasVer   = true;
    }

    // 2. Check Advertised Custom 128-bit Brand Service UUID
    const bool uuidMatch = info.serviceUuids().contains(OnyxOta::kServiceUuid);

    return mfgMatch || uuidMatch;
}

bool SpeakerScanner::matchesOnyxClassic(const QBluetoothDeviceInfo &info,
                                        quint8 &modelLo, quint8 &maj, quint8 &min, quint8 &pat,
                                        bool &hasVer) const
{
    hasVer = false;

    const QByteArray mfg = info.manufacturerData(OnyxOta::kManufacturerCompanyId);
    if (mfg.size() >= 6 &&
        static_cast<quint8>(mfg[0]) == OnyxOta::kMfgMagic0 &&
        static_cast<quint8>(mfg[1]) == OnyxOta::kMfgMagic1)
    {
        modelLo = static_cast<quint8>(mfg[2]);
        maj     = static_cast<quint8>(mfg[3]);
        min     = static_cast<quint8>(mfg[4]);
        pat     = static_cast<quint8>(mfg[5]);
        hasVer  = true;
        return true;
    }

    if (info.serviceUuids().contains(OnyxOta::kServiceUuid)) {
        return true;
    }

    return false;
}

bool SpeakerScanner::areLikelySameEsp32(const QBluetoothAddress &a, const QBluetoothAddress &b) {
    if (a.isNull() || b.isNull()) return false;
    if (a == b) return true;
    const quint64 va = a.toUInt64();
    const quint64 vb = b.toUInt64();
    // ESP32 assigns Base MAC (WiFi), Base+1 (SoftAP), Base+2 (BT Classic), Base+3 (BLE)
    // or Classic == BLE +/- 2 within the same OUI + NIC prefix.
    if ((va & ~0xFFULL) == (vb & ~0xFFULL)) {
        const int diff = std::abs(static_cast<int>(va & 0xFFULL) - static_cast<int>(vb & 0xFFULL));
        return diff <= 3;
    }
    return false;
}

int SpeakerScanner::findMatchingSpeakerIndex(const QBluetoothAddress &addr, const QString &name) const {
    for (int i = 0; i < m_devices.size(); ++i) {
        const auto &d = m_devices.at(i);
        if (!addr.isNull()) {
            if (areLikelySameEsp32(d.bleAddress, addr) || areLikelySameEsp32(d.classicAddress, addr)) {
                return i;
            }
        }
        if (!name.isEmpty() && d.name == name && addr.isNull()) {
            return i;
        }
    }
    return -1;
}

void SpeakerScanner::upsertSpeaker(const SpeakerDevice &cand) {
    const QBluetoothAddress probeAddr = cand.hasClassic ? cand.classicAddress : cand.bleAddress;
    const int existing = findMatchingSpeakerIndex(probeAddr, cand.name);

    if (existing >= 0) {
        SpeakerDevice &d = m_devices[existing];
        if (!cand.name.isEmpty() && d.name.isEmpty()) d.name = cand.name;
        if (cand.hasBle) {
            d.hasBle     = true;
            d.bleInfo    = cand.bleInfo;
            d.bleAddress = cand.bleAddress;
        }
        if (cand.hasClassic) {
            d.hasClassic     = true;
            d.classicAddress = cand.classicAddress;
            d.pairedClassic  = d.pairedClassic || cand.pairedClassic;
        }
        if (cand.hasAdvVersion) {
            d.hasAdvVersion = true;
            d.modelIdLo     = cand.modelIdLo;
            d.fwMajor       = cand.fwMajor;
            d.fwMinor       = cand.fwMinor;
            d.fwPatch       = cand.fwPatch;
        }
        if (cand.rssi != 0) d.rssi = cand.rssi;

        const QModelIndex idx = index(existing);
        emit dataChanged(idx, idx);
        return;
    }

    beginInsertRows(QModelIndex(), m_devices.size(), m_devices.size());
    m_devices.append(cand);
    endInsertRows();
    emit countChanged();

    setStatus(QStringLiteral("Found %1 Onyx Studio speaker(s)").arg(m_devices.size()));
}

void SpeakerScanner::onDeviceDiscovered(const QBluetoothDeviceInfo &info) {
    const auto cfg = info.coreConfigurations();
    const bool isBle     = cfg.testFlag(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
    const bool isClassic = cfg.testFlag(QBluetoothDeviceInfo::BaseRateCoreConfiguration) ||
                           cfg.testFlag(QBluetoothDeviceInfo::BaseRateAndLowEnergyCoreConfiguration);

    quint8 modelLo = 0, maj = 0, min = 0, pat = 0;
    bool hasVer = false;

    if (isBle && matchesOnyxBle(info, modelLo, maj, min, pat, hasVer)) {
        SpeakerDevice dev;
        dev.id            = !info.address().isNull() ? info.address().toString()
                                                     : info.deviceUuid().toString();
        dev.name          = info.name().isEmpty() ? QStringLiteral("HK Onyx Studio") : info.name();
        dev.bleInfo       = info;
        dev.bleAddress    = info.address();
        dev.hasBle        = true;
        dev.modelIdLo     = modelLo;
        dev.fwMajor       = maj;
        dev.fwMinor       = min;
        dev.fwPatch       = pat;
        dev.hasAdvVersion = hasVer;
        dev.rssi          = info.rssi();
        upsertSpeaker(dev);
    }

    if (isClassic && matchesOnyxClassic(info, modelLo, maj, min, pat, hasVer)) {
        SpeakerDevice dev;
        dev.id             = info.address().toString();
        dev.name           = info.name().isEmpty() ? QStringLiteral("HK Onyx Studio") : info.name();
        dev.classicAddress = info.address();
        dev.hasClassic     = true;
        dev.pairedClassic  = (m_localDev && m_localDev->isValid() &&
                              m_localDev->pairingStatus(info.address()) != QBluetoothLocalDevice::Unpaired);
        dev.modelIdLo      = modelLo;
        dev.fwMajor        = maj;
        dev.fwMinor        = min;
        dev.fwPatch        = pat;
        dev.hasAdvVersion  = hasVer;
        dev.rssi           = info.rssi();
        upsertSpeaker(dev);
    }
}

void SpeakerScanner::onDeviceUpdated(const QBluetoothDeviceInfo &info, QBluetoothDeviceInfo::Fields) {
    // Re-evaluate updated advertisements (e.g. when scan response arrives with name or manufacturer data)
    onDeviceDiscovered(info);
}

void SpeakerScanner::onDiscoveryFinished() {
    m_scanning = false;
    emit scanningChanged();
    if (m_devices.isEmpty()) {
        setStatus(QStringLiteral("No Onyx Studio speakers found. Ensure speaker is powered on."));
    } else {
        setStatus(QStringLiteral("Found %1 Onyx Studio speaker(s)").arg(m_devices.size()));
    }
}

void SpeakerScanner::onDiscoveryError(QBluetoothDeviceDiscoveryAgent::Error error) {
    Q_UNUSED(error);
    m_scanning = false;
    emit scanningChanged();
    setStatus(QStringLiteral("Scan error: %1").arg(m_agent->errorString()));
    emit scanError(m_statusMessage);
}

void SpeakerScanner::probeConnectedClassicDevices() {
    if (!m_localDev || !m_localDev->isValid()) return;
    const auto connected = m_localDev->connectedDevices();
    for (const QBluetoothAddress &addr : connected) {
        const QString key = addr.toString();
        if (!m_probedSdpAddrs.contains(key)) {
            m_probedSdpAddrs.insert(key);
            m_sdpQueue.append(addr);
        }
    }
    startNextSdpProbe();
}

void SpeakerScanner::startNextSdpProbe() {
    if (m_sdpAgent || m_sdpQueue.isEmpty()) return;

    const QBluetoothAddress addr = m_sdpQueue.takeFirst();
    m_sdpAgent = new QBluetoothServiceDiscoveryAgent(this);
    m_sdpAgent->setRemoteAddress(addr);
    m_sdpAgent->setUuidFilter(QBluetoothUuid(QBluetoothUuid::ServiceClassUuid::SerialPort));

    connect(m_sdpAgent, &QBluetoothServiceDiscoveryAgent::serviceDiscovered,
            this, &SpeakerScanner::onSdpServiceDiscovered);
    connect(m_sdpAgent, &QBluetoothServiceDiscoveryAgent::finished,
            this, &SpeakerScanner::onSdpFinished);
    connect(m_sdpAgent, &QBluetoothServiceDiscoveryAgent::canceled,
            this, &SpeakerScanner::onSdpFinished);

    m_sdpAgent->start(QBluetoothServiceDiscoveryAgent::MinimalDiscovery);
}

void SpeakerScanner::onSdpServiceDiscovered(const QBluetoothServiceInfo &service) {
    // Strictly match ONLY speakers advertising our custom Onyx SPP SDP service name!
    if (service.serviceName() != OnyxOta::kSppServiceName) {
        return;
    }

    const QBluetoothDeviceInfo devInfo = service.device();
    SpeakerDevice dev;
    dev.id             = devInfo.address().toString();
    dev.name           = devInfo.name().isEmpty() ? QStringLiteral("HK Onyx Studio") : devInfo.name();
    dev.classicAddress = devInfo.address();
    dev.hasClassic     = true;
    dev.pairedClassic  = true;
    upsertSpeaker(dev);
}

void SpeakerScanner::onSdpFinished() {
    if (m_sdpAgent) {
        m_sdpAgent->deleteLater();
        m_sdpAgent = nullptr;
    }
    startNextSdpProbe();
}

void SpeakerScanner::setStatus(const QString &msg) {
    if (m_statusMessage != msg) {
        m_statusMessage = msg;
        emit statusMessageChanged();
    }
}
