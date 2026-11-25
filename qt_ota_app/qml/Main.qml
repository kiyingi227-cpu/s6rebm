import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    width: 1020
    height: 760
    minimumWidth: 360
    minimumHeight: 620
    title: "Onyx Studio Companion — Dual-Mode OTA"

    Material.theme: Material.Dark
    Material.accent: "#D9B382"      // Warm Harman champagne gold
    Material.primary: "#1C1E24"
    Material.background: "#111317"

    readonly property bool isCompact: width < 760
    readonly property color bgCard: "#1A1D24"
    readonly property color bgCardAlt: "#222631"
    readonly property color borderSubtle: "#2C3140"
    readonly property color textPrimary: "#F4F5F7"
    readonly property color textSecondary: "#9AA2B2"
    readonly property color accentGold: "#D9B382"
    readonly property color accentGreen: "#4CD98B"
    readonly property color accentBlue: "#64B5F6"
    readonly property color accentRed: "#FF6B6B"

    FileDialog {
        id: fwFileDialog
        title: "Select Onyx Studio Firmware (.bin)"
        // Android SAF filters by MIME type and can gray out .bin files if restricted to *.bin;
        // C++ parseAndValidateFirmwareFile() strictly enforces 0xE9 + ONYX_FW_DESC_V1.
        nameFilters: Qt.platform.os === "android"
                     ? ["All files (*)"]
                     : ["ESP32 Firmware (*.bin)", "All files (*)"]
        onAccepted: ota.loadFirmwareFile(selectedFile)
    }

    header: ToolBar {
        height: 64
        background: Rectangle {
            color: "#15181E"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: borderSubtle
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 12

            Image {
                source: "qrc:/qt/qml/OnyxOta/assets/app_icon.png"
                sourceSize.width: 72
                sourceSize.height: 72
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                fillMode: Image.PreserveAspectFit
                smooth: true
            }

            ColumnLayout {
                spacing: 1
                Layout.fillWidth: true
                Label {
                    text: "ONYX STUDIO OTA"
                    color: textPrimary
                    font.bold: true
                    font.pixelSize: 16
                    font.letterSpacing: 1.2
                }
                Label {
                    text: ota.connected
                          ? (ota.speakerModel + " • " + ota.activeTransportName)
                          : "Exclusive Onyx Brand Filter (BLE + Classic SPP)"
                    color: ota.connected ? accentGreen : textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Button {
                text: ota.scanner.scanning ? "Scanning..." : "Scan"
                enabled: !ota.otaInProgress
                highlighted: true
                onClicked: ota.startScan()
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 16

            // Permission warning banner (shown on Android if denied/pending)
            Rectangle {
                visible: !ota.bluetoothPermissionGranted
                Layout.fillWidth: true
                Layout.margins: 16
                Layout.bottomMargin: 0
                implicitHeight: permRow.implicitHeight + 20
                radius: 12
                color: "#2E1F22"
                border.color: accentRed

                RowLayout {
                    id: permRow
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 12
                    Label {
                        text: ota.permissionStatus
                        color: textPrimary
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Grant"
                        onClicked: ota.requestBluetoothPermissions()
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                columns: isCompact ? 1 : 2
                columnSpacing: 16
                rowSpacing: 16

                // ============================================================
                // CARD 1: Brand-Filtered Speaker Discovery (BLE + Classic)
                // ============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: discoveryCol.implicitHeight + 32
                    radius: 16
                    color: bgCard
                    border.color: borderSubtle

                    ColumnLayout {
                        id: discoveryCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "Discovered Onyx Speakers"
                                color: textPrimary
                                font.bold: true
                                font.pixelSize: 16
                                Layout.fillWidth: true
                            }
                            BusyIndicator {
                                running: ota.scanner.scanning
                                visible: running
                                implicitWidth: 22
                                implicitHeight: 22
                            }
                        }

                        Label {
                            text: ota.scanner.statusMessage
                            color: textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        // Preferred link selector
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Label {
                                text: "Preferred Link:"
                                color: textSecondary
                                font.pixelSize: 12
                            }
                            ComboBox {
                                Layout.fillWidth: true
                                enabled: !ota.otaInProgress
                                model: ["Auto (Classic SPP if paired, else BLE)", "Bluetooth Classic SPP (Fast)", "Bluetooth Low Energy (BLE GATT)"]
                                currentIndex: ota.transportPreference
                                onActivated: ota.transportPreference = currentIndex
                            }
                        }

                        // Empty state
                        Rectangle {
                            visible: ota.scanner.count === 0
                            Layout.fillWidth: true
                            implicitHeight: 92
                            radius: 12
                            color: bgCardAlt
                            border.color: borderSubtle

                            ColumnLayout {
                                anchors.centerIn: parent
                                width: parent.width - 24
                                spacing: 4
                                Label {
                                    text: ota.scanner.scanning
                                          ? "Listening for Onyx Studio beacons..."
                                          : "No Onyx Studio speakers detected yet"
                                    color: textPrimary
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: "Non-Onyx Bluetooth devices are automatically hidden (filtered by 128-bit brand UUID & manufacturer signature)."
                                    color: textSecondary
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                    horizontalAlignment: Text.AlignHCenter
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        // Discovered speaker cards
                        Repeater {
                            model: ota.scanner
                            delegate: Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: devRow.implicitHeight + 20
                                radius: 12
                                color: (ota.connected && ota.connectedDeviceId === model.deviceId) ? "#232B26" : bgCardAlt
                                border.color: (ota.connected && ota.connectedDeviceId === model.deviceId) ? accentGreen : borderSubtle

                                RowLayout {
                                    id: devRow
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 10

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        Label {
                                            text: model.name
                                            color: textPrimary
                                            font.bold: true
                                            font.pixelSize: 14
                                        }
                                        Label {
                                            text: model.displayAddress + "  •  FW " + model.advertisedVersion +
                                                  (model.rssi !== 0 ? ("  •  " + model.rssi + " dBm") : "")
                                            color: textSecondary
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        RowLayout {
                                            spacing: 6
                                            Rectangle {
                                                visible: model.hasBle
                                                radius: 6
                                                color: "#1B2E40"
                                                implicitWidth: bleLbl.implicitWidth + 12
                                                implicitHeight: 20
                                                Label {
                                                    id: bleLbl
                                                    anchors.centerIn: parent
                                                    text: "BLE"
                                                    color: accentBlue
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                }
                                            }
                                            Rectangle {
                                                visible: model.hasClassic
                                                radius: 6
                                                color: "#2E281B"
                                                implicitWidth: clsLbl.implicitWidth + 12
                                                implicitHeight: 20
                                                Label {
                                                    id: clsLbl
                                                    anchors.centerIn: parent
                                                    text: model.pairedClassic ? "CLASSIC SPP • PAIRED" : "CLASSIC SPP"
                                                    color: accentGold
                                                    font.pixelSize: 10
                                                    font.bold: true
                                                }
                                            }
                                        }
                                    }

                                    Button {
                                        text: (ota.connected && ota.connectedDeviceId === model.deviceId)
                                              ? "Disconnect"
                                              : (ota.connecting ? "..." : "Connect")
                                        enabled: !ota.otaInProgress && !ota.connecting
                                        highlighted: !(ota.connected && ota.connectedDeviceId === model.deviceId)
                                        onClicked: {
                                            if (ota.connected && ota.connectedDeviceId === model.deviceId) {
                                                ota.disconnectSpeaker()
                                            } else {
                                                ota.connectToDevice(model.deviceId)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ============================================================
                // CARD 2: Live Speaker Status & Dual-Transport Controls
                // ============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: statusCol.implicitHeight + 32
                    radius: 16
                    color: bgCard
                    border.color: ota.connected ? accentGold : borderSubtle

                    ColumnLayout {
                        id: statusCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "Speaker Status"
                                color: textPrimary
                                font.bold: true
                                font.pixelSize: 16
                                Layout.fillWidth: true
                            }
                            Button {
                                visible: ota.connected
                                enabled: !ota.otaInProgress
                                text: "Refresh"
                                flat: true
                                onClicked: ota.refreshSpeakerInfo()
                            }
                        }

                        Label {
                            visible: !ota.connected
                            text: ota.connecting
                                  ? "Connecting and verifying Onyx Studio signature..."
                                  : "Connect to a discovered Onyx Studio speaker to inspect firmware version, OTA partition slot, and battery health."
                            color: textSecondary
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        GridLayout {
                            visible: ota.connected
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 12
                            rowSpacing: 8

                            Label { text: "Model"; color: textSecondary; font.pixelSize: 12 }
                            Label { text: ota.speakerModel; color: textPrimary; font.bold: true; Layout.fillWidth: true }

                            Label { text: "Firmware"; color: textSecondary; font.pixelSize: 12 }
                            Label { text: ota.speakerVersion + " (" + ota.speakerBuildInfo + ")"; color: accentGold; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }

                            Label { text: "Active Slot"; color: textSecondary; font.pixelSize: 12 }
                            Label { text: ota.runningSlot + "  •  Max image " + Math.floor(ota.maxOtaSize / 1024) + " KB"; color: textPrimary; Layout.fillWidth: true }

                            Label { text: "Battery / Power"; color: textSecondary; font.pixelSize: 12 }
                            Label {
                                text: ota.batteryPercent + "% (" + ota.batteryVolts.toFixed(2) + " V)" +
                                      (ota.dcPresent ? (ota.charging ? " • AC Charging" : " • 19V AC") :
                                      (ota.lowBattery ? " • LOW BATTERY" : " • Battery"))
                                color: ota.lowBattery && !ota.dcPresent ? accentRed : accentGreen
                                font.bold: true
                                Layout.fillWidth: true
                            }

                            Label { text: "Active Transport"; color: textSecondary; font.pixelSize: 12 }
                            Label { text: ota.activeTransportName + "  (" + ota.classicMac + ")"; color: accentBlue; elide: Text.ElideRight; Layout.fillWidth: true }
                        }

                        RowLayout {
                            visible: ota.connected && (ota.canSwitchToClassic || ota.canSwitchToBle)
                            Layout.fillWidth: true
                            spacing: 8

                            Button {
                                visible: ota.canSwitchToClassic
                                text: "Switch to Fast Classic SPP"
                                Layout.fillWidth: true
                                onClicked: ota.switchTransport(1)
                            }
                            Button {
                                visible: ota.canSwitchToBle
                                text: "Switch to BLE GATT"
                                Layout.fillWidth: true
                                onClicked: ota.switchTransport(2)
                            }
                        }
                    }
                }

                // ============================================================
                // CARD 3: Firmware File Selector & Verification
                // ============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: fileCol.implicitHeight + 32
                    radius: 16
                    color: bgCard
                    border.color: borderSubtle

                    ColumnLayout {
                        id: fileCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "Firmware File Selection"
                                color: textPrimary
                                font.bold: true
                                font.pixelSize: 16
                                Layout.fillWidth: true
                            }
                            Button {
                                text: "Select .bin"
                                enabled: !ota.otaInProgress
                                highlighted: true
                                onClicked: fwFileDialog.open()
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: fileInfoCol.implicitHeight + 20
                            radius: 12
                            color: bgCardAlt
                            border.color: !ota.fileLoaded ? borderSubtle
                                        : (ota.fileValid && ota.fileCompatibleWithSpeaker ? accentGreen : accentRed)

                            ColumnLayout {
                                id: fileInfoCol
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 4

                                Label {
                                    text: ota.fileLoaded ? ota.fileName : "No firmware binary chosen"
                                    color: textPrimary
                                    font.bold: true
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }

                                Label {
                                    visible: ota.fileValid
                                    text: ota.fileModelName + "  •  " + ota.fileVersion +
                                          "  •  " + Math.floor(ota.fileSizeBytes / 1024) + " KB  •  SHA-256 " + ota.fileSha256Short
                                    color: accentGold
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }

                                Label {
                                    text: ota.fileCompatibilityText
                                    color: !ota.fileLoaded ? textSecondary
                                         : (ota.fileValid && ota.fileCompatibleWithSpeaker ? accentGreen : accentRed)
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }

                // ============================================================
                // CARD 4: OTA Progress, Controls & Transfer Log
                // ============================================================
                Rectangle {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: otaCol.implicitHeight + 32
                    radius: 16
                    color: bgCard
                    border.color: ota.otaInProgress ? accentGold : borderSubtle

                    ColumnLayout {
                        id: otaCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: "OTA Update Progress"
                                color: textPrimary
                                font.bold: true
                                font.pixelSize: 16
                                Layout.fillWidth: true
                            }
                            Rectangle {
                                radius: 8
                                color: ota.otaPhase === "Complete" ? "#1E3A2A"
                                     : ota.otaPhase === "Error"    ? "#3D1F24"
                                     : ota.otaInProgress           ? "#332B1A" : "#252934"
                                implicitWidth: phaseLbl.implicitWidth + 16
                                implicitHeight: 24
                                Label {
                                    id: phaseLbl
                                    anchors.centerIn: parent
                                    text: ota.otaPhase.toUpperCase()
                                    color: ota.otaPhase === "Complete" ? accentGreen
                                         : ota.otaPhase === "Error"    ? accentRed
                                         : accentGold
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }
                        }

                        Label {
                            text: ota.otaStatusText
                            color: textSecondary
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0.0
                            to: 1.0
                            value: ota.progress
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: Math.round(ota.progress * 100) + "%  (" +
                                      Math.floor(ota.bytesSent / 1024) + " / " +
                                      Math.floor(ota.totalBytes / 1024) + " KB)"
                                color: textPrimary
                                font.pixelSize: 12
                                font.bold: true
                                Layout.fillWidth: true
                            }
                            Label {
                                visible: ota.otaInProgress && ota.speedKBps > 0.1
                                text: ota.speedKBps.toFixed(1) + " KB/s  •  ETA " + ota.etaSeconds + " s"
                                color: accentGold
                                font.pixelSize: 12
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            Button {
                                text: "Install Firmware Update"
                                Layout.fillWidth: true
                                highlighted: true
                                enabled: ota.canStartOta
                                onClicked: ota.startOta()
                            }

                            Button {
                                text: "Abort"
                                enabled: ota.otaInProgress
                                onClicked: ota.abortOta()
                            }
                        }

                        // Compact transfer log
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 140
                            radius: 10
                            color: "#0E1014"
                            border.color: borderSubtle

                            ListView {
                                id: logList
                                anchors.fill: parent
                                anchors.margins: 8
                                clip: true
                                model: ota.logs
                                onCountChanged: positionViewAtEnd()
                                delegate: Label {
                                    width: logList.width
                                    text: modelData
                                    color: "#B6BECE"
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    wrapMode: Text.WrapAnywhere
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
