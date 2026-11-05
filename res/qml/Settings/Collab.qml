import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mixxx 1.0 as Mixxx
import ".." as Skin
import "../Theme"

Category {
    id: root

    property int playTestLeadMs: 3000

    function startProbe() {
        Mixxx.Collab.startProbe();
    }

    label: "Online B2B"

    ScrollView {
        id: scrollView

        anchors.fill: parent

        ColumnLayout {
            spacing: 14
            width: scrollView.width - 24

            Mixxx.SettingGroup {
                Layout.fillWidth: true
                label: "Connection"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Text {
                        Layout.fillWidth: true
                        color: Theme.textColor
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        text: qsTr("Clock probe between two Mixxx instances. Use the same session on two machines, or two instances on one machine with separate settings paths.")
                        wrapMode: Text.WordWrap
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columnSpacing: 12
                        columns: 2
                        rowSpacing: 8

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Peer address")
                        }
                        Skin.TextField {
                            Layout.preferredWidth: 240
                            enabled: !Mixxx.Collab.probeRunning
                            placeholderText: "127.0.0.1"
                            text: Mixxx.Collab.peerHost

                            onEditingFinished: Mixxx.Collab.peerHost = text
                        }

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Peer port")
                        }
                        Skin.TextField {
                            Layout.preferredWidth: 240
                            enabled: !Mixxx.Collab.probeRunning
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: Mixxx.Collab.peerPort
                            validator: IntValidator {
                                bottom: 1
                                top: 65535
                            }

                            onEditingFinished: Mixxx.Collab.peerPort = Number(text)
                        }

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Local port")
                        }
                        Skin.TextField {
                            Layout.preferredWidth: 240
                            enabled: !Mixxx.Collab.probeRunning
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: Mixxx.Collab.localPort
                            validator: IntValidator {
                                bottom: 1
                                top: 65535
                            }

                            onEditingFinished: Mixxx.Collab.localPort = Number(text)
                        }

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Probe interval")
                        }
                        RowLayout {
                            spacing: 6

                            Skin.TextField {
                                Layout.preferredWidth: 100
                                enabled: !Mixxx.Collab.probeRunning
                                inputMethodHints: Qt.ImhDigitsOnly
                                text: Mixxx.Collab.intervalMs
                                validator: IntValidator {
                                    bottom: 20
                                    top: 5000
                                }

                                onEditingFinished: Mixxx.Collab.intervalMs = Number(text)
                            }
                            Text {
                                color: Theme.textColor
                                font.family: Theme.fontFamily
                                font.pixelSize: 13
                                text: qsTr("ms")
                            }
                        }

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Role")
                        }
                        RatioChoice {
                            maxWidth: 240
                            options: ["host", "guest"]
                            selected: Mixxx.Collab.hostRole ? "host" : "guest"

                            onSelectedChanged: {
                                Mixxx.Collab.hostRole = selected === "host";
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        color: Theme.midGray2
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        text: qsTr("Swap the ports on the two peers: the peer port of one machine is the local port of the other.")
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Mixxx.SettingGroup {
                Layout.fillWidth: true
                label: "Status"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    RowLayout {
                        spacing: 8

                        Rectangle {
                            color: Mixxx.Collab.probeRunning ? Theme.accentColor : Theme.midGray3
                            height: 10
                            radius: 5
                            width: 10
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: Mixxx.Collab.probeRunning ? qsTr("Probe running") : qsTr("Probe stopped")
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        Text {
                            color: Theme.textColor
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            text: qsTr("%1 samples").arg(Mixxx.Collab.sampleCount)
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columnSpacing: 32
                        columns: 5
                        rowSpacing: 4

                        Text {
                            color: Theme.midGray2
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            text: qsTr("RTT")
                        }
                        Text {
                            color: Theme.midGray2
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            text: qsTr("Offset")
                        }
                        Text {
                            color: Theme.midGray2
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            text: qsTr("Jitter")
                        }
                        Text {
                            color: Theme.midGray2
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            text: qsTr("Skew")
                        }
                        Text {
                            color: Theme.midGray2
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            text: qsTr("Accepted")
                        }

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            text: Mixxx.Collab.rttMs.toFixed(2) + " ms"
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            text: Mixxx.Collab.offsetMs.toFixed(2) + " ms"
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            text: Mixxx.Collab.jitterMs.toFixed(2) + " ms"
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            text: Mixxx.Collab.skewPpm.toFixed(2) + " ppm"
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 16
                            text: Mixxx.Collab.sampleCount
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        color: Theme.red
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        text: Mixxx.Collab.lastError
                        visible: Mixxx.Collab.lastError.length > 0
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Mixxx.SettingGroup {
                Layout.fillWidth: true
                label: "Diagnostics"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    GridLayout {
                        Layout.fillWidth: true
                        columnSpacing: 12
                        columns: 2
                        rowSpacing: 8

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("CSV log")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Skin.TextField {
                                Layout.fillWidth: true
                                enabled: !Mixxx.Collab.probeRunning
                                text: Mixxx.Collab.diagnosticsPath

                                onEditingFinished: Mixxx.Collab.diagnosticsPath = text
                            }
                            Skin.FormButton {
                                enabled: !Mixxx.Collab.probeRunning
                                text: qsTr("Default")

                                onClicked: Mixxx.Collab.diagnosticsPath = Mixxx.Collab.defaultDiagnosticsPath()
                            }
                        }
                    }

                    RowLayout {
                        spacing: 8

                        Skin.FormButton {
                            enabled: !Mixxx.Collab.probeRunning
                            text: qsTr("Start probe")

                            onClicked: root.startProbe()
                        }
                        Skin.FormButton {
                            enabled: Mixxx.Collab.probeRunning
                            text: qsTr("Stop probe")

                            onClicked: Mixxx.Collab.stopProbe()
                        }
                        Skin.FormButton {
                            enabled: Mixxx.Collab.sampleCount > 0
                            text: qsTr("Clear samples")

                            onClicked: Mixxx.Collab.clearStatistics()
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                }
            }

            Mixxx.SettingGroup {
                Layout.fillWidth: true
                label: "Scheduling test"

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    RowLayout {
                        spacing: 10

                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Deck")
                        }
                        RatioChoice {
                            id: playTestDeck

                            maxWidth: 160
                            options: ["1", "2", "3", "4"]
                        }
                        Text {
                            color: Theme.white
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                            text: qsTr("Lead")
                        }
                        Skin.TextField {
                            Layout.preferredWidth: 90
                            inputMethodHints: Qt.ImhDigitsOnly
                            text: root.playTestLeadMs
                            validator: IntValidator {
                                bottom: 200
                                top: 20000
                            }

                            onEditingFinished: root.playTestLeadMs = Number(text)
                        }
                        Text {
                            color: Theme.textColor
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            text: qsTr("ms")
                        }
                        Skin.FormButton {
                            text: qsTr("Schedule play")

                            onClicked: Mixxx.Collab.schedulePlayTest(Number(playTestDeck.selected), root.playTestLeadMs)
                        }
                        Skin.FormButton {
                            enabled: Mixxx.Collab.scheduledCommandValid
                            text: qsTr("Clear result")

                            onClicked: Mixxx.Collab.cancelScheduledCommands()
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        color: Mixxx.Collab.scheduledCommandValid ? Theme.white : Theme.midGray2
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        text: Mixxx.Collab.scheduledCommandValid ? qsTr("Deck %1 played %2 ms after the target").arg(Mixxx.Collab.lastCommandDeck).arg(Mixxx.Collab.lastCommandLatenessMs.toFixed(2)) : qsTr("The play command is aligned to the next two-second session-time boundary.")
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        color: Theme.midGray2
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        text: qsTr("The command timing is logged. Measure the actual audio onset by recording both outputs on one device.")
                        wrapMode: Text.WordWrap
                    }
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
