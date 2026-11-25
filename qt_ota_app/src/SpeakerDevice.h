#pragma once

#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
#include <QString>
#include <cstdint>

struct SpeakerDevice {
    QString              id;             // Canonical key (Classic MAC if known, else BLE address/UUID)
    QString              name;
    QBluetoothDeviceInfo bleInfo;
    QBluetoothAddress    bleAddress;
    QBluetoothAddress    classicAddress;
    bool                 hasBle          = false;
    bool                 hasClassic      = false;
    bool                 pairedClassic   = false;
    quint8               modelIdLo       = 0;
    quint8               fwMajor         = 0;
    quint8               fwMinor         = 0;
    quint8               fwPatch         = 0;
    bool                 hasAdvVersion   = false;
    qint16               rssi            = 0;

    QString advertisedVersion() const {
        if (!hasAdvVersion) return QStringLiteral("Unknown");
        return QStringLiteral("v%1.%2.%3").arg(fwMajor).arg(fwMinor).arg(fwPatch);
    }

    QString displayAddress() const {
        if (hasClassic && !classicAddress.isNull()) return classicAddress.toString();
        if (hasBle && !bleAddress.isNull())         return bleAddress.toString();
        return id;
    }
};
