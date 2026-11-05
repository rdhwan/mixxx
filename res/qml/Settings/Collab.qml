import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mixxx 1.0 as Mixxx
import ".." as Skin
import "../Theme"

Category {
    id: root

    property int playTestLeadMs: 3000

    function schedulePlayTest() {
        Mixxx.Collab.schedulePlayTest(Number(playTestDeck.selected), root.playTestLeadMs);
    }

    label: "Online B2B"

    component SectionTitle: Text {
        Layout.fillWidth: true
        Layout.leftMargin: 6
        color: Theme.white
        font.family: Theme.fontFamily
        font.pixelSize: 16
        font.weight: Font.DemiBold
    }

    component Panel: Rectangle {
        id: panel

        default property alias content: panelColumn.data

        Layout.fillWidth: true
        border.color: Theme.darkGray3
        border.width: 1
        color: Theme.darkGray4
        implicitHeight: panelColumn.implicitHeight + 28
        radius: 4

        ColumnLayout {
            id: panelColumn

            anchors.fill: parent
            anchors.margins: 14
            spacing: 12
        }
    }

    component FieldLabel: Text {
        color: Theme.white
        font.family: Theme.fontFamily
        font.pixelSize: 14
        verticalAlignment: Text.AlignVCenter
    }

    component Hint: Text {
        Layout.fillWidth: true
        color: Theme.midGray
        font.family: Theme.fontFamily
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    component Metric: ColumnLayout {
        id: metric

        property string label: ""
        property string value: ""

        Layout.fillWidth: true
        spacing: 2

        Text {
            Layout.fillWidth: true
            color: Theme.midGray
            font.family: Theme.fontFamily
            font.pixelSize: 11
            text: metric.label
        }
        Text {
            Layout.fillWidth: true
            color: Theme.white
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 18
            font.weight: Font.DemiBold
            text: metric.value
        }
    }

    ScrollView {
        id: scrollView

        anchors.fill: parent
        clip: true

        ColumnLayout {
            spacing: 22
            width: root.width - 26

            SectionTitle {
                text: qsTr("Connection")
            }
            Panel {
                GridLayout {
                    Layout.fillWidth: true
                    columnSpacing: 16
                    columns: 2
                    rowSpacing: 10

                    FieldLabel {
                        text: qsTr("Peer address")

                        Mixxx.SettingParameter {
                            label: qsTr("Peer address")
                        }
                    }
                    Skin.TextField {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 220
                        enabled: !Mixxx.Collab.probeRunning
                        placeholderText: "127.0.0.1"
                        text: Mixxx.Collab.peerHost

                        onEditingFinished: Mixxx.Collab.peerHost = text
                    }

                    FieldLabel {
                        text: qsTr("Peer port")

                        Mixxx.SettingParameter {
                            label: qsTr("Peer port")
                        }
                    }
                    Skin.TextField {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 220
                        enabled: !Mixxx.Collab.probeRunning
                        inputMethodHints: Qt.ImhDigitsOnly
                        text: Mixxx.Collab.peerPort
                        validator: IntValidator {
                            bottom: 1
                            top: 65535
                        }

                        onEditingFinished: Mixxx.Collab.peerPort = Number(text)
                    }

                    FieldLabel {
                        text: qsTr("Local port")

                        Mixxx.SettingParameter {
                            label: qsTr("Local port")
                        }
                    }
                    Skin.TextField {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 220
                        enabled: !Mixxx.Collab.probeRunning
                        inputMethodHints: Qt.ImhDigitsOnly
                        text: Mixxx.Collab.localPort
                        validator: IntValidator {
                            bottom: 1
                            top: 65535
                        }

                        onEditingFinished: Mixxx.Collab.localPort = Number(text)
                    }

                    FieldLabel {
                        text: qsTr("Probe interval")

                        Mixxx.SettingParameter {
                            label: qsTr("Probe interval")
                        }
                    }
                    RowLayout {
                        spacing: 6

                        Skin.TextField {
                            Layout.preferredWidth: 80
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

                    FieldLabel {
                        text: qsTr("Session role")

                        Mixxx.SettingParameter {
                            label: qsTr("Session role")
                        }
                    }
                    RatioChoice {
                        Layout.preferredWidth: 220
                        enabled: !Mixxx.Collab.probeRunning
                        options: ["host", "guest"]
                        selected: Mixxx.Collab.hostRole ? "host" : "guest"

                        onSelectedChanged: {
                            Mixxx.Collab.hostRole = selected === "host";
                        }
                    }
                }

                Hint {
                    text: qsTr("Host defines session time; guest maps its local clock onto the host clock. Swap the ports on the two peers: the peer port of one machine is the local port of the other.")
                }
            }

            SectionTitle {
                text: qsTr("Measurements")
            }
            Panel {
                RowLayout {
                    spacing: 8

                    Rectangle {
                        color: Mixxx.Collab.probeRunning ? Theme.green : Theme.midGray3
                        height: 10
                        radius: 5
                        width: 10
                    }
                    Text {
                        color: Theme.white
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        text: Mixxx.Collab.probeRunning ? qsTr("Probe running") : qsTr("Probe stopped")
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Text {
                        color: Theme.textColor
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        text: qsTr("%1 samples").arg(Mixxx.Collab.sampleCount)
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columnSpacing: 24
                    columns: 4
                    rowSpacing: 8

                    Metric {
                        label: qsTr("Round trip")
                        value: Mixxx.Collab.rttMs.toFixed(2) + qsTr(" ms")
                    }
                    Metric {
                        label: qsTr("Clock offset")
                        value: Mixxx.Collab.offsetMs.toFixed(2) + qsTr(" ms")
                    }
                    Metric {
                        label: qsTr("Jitter")
                        value: Mixxx.Collab.jitterMs.toFixed(2) + qsTr(" ms")
                    }
                    Metric {
                        label: qsTr("Clock skew")
                        value: Mixxx.Collab.skewPpm.toFixed(2) + qsTr(" ppm")
                    }
                }

                RowLayout {
                    spacing: 8

                    Skin.FormButton {
                        enabled: !Mixxx.Collab.probeRunning
                        text: qsTr("Start probe")

                        onClicked: Mixxx.Collab.startProbe()
                    }
                    Skin.FormButton {
                        enabled: Mixxx.Collab.probeRunning
                        text: qsTr("Stop probe")

                        onClicked: Mixxx.Collab.stopProbe()
                    }
                    Skin.FormButton {
                        enabled: !Mixxx.Collab.probeRunning && Mixxx.Collab.sampleCount > 0
                        text: qsTr("Clear samples")

                        onClicked: Mixxx.Collab.clearStatistics()
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }

                Hint {
                    text: Mixxx.Collab.probeRunning ? qsTr("Leave the probe running for ten minutes before judging offset stability.") : qsTr("Start the probe on both peers. The estimate becomes usable after a few exchanges.")
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

            SectionTitle {
                text: qsTr("Scheduling test")
            }
            Panel {
                RowLayout {
                    spacing: 12

                    FieldLabel {
                        text: qsTr("Deck")

                        Mixxx.SettingParameter {
                            label: qsTr("Scheduling test deck")
                        }
                    }
                    RatioChoice {
                        id: playTestDeck

                        Layout.preferredWidth: 160
                        options: ["1", "2", "3", "4"]
                    }
                    FieldLabel {
                        text: qsTr("Lead")

                        Mixxx.SettingParameter {
                            label: qsTr("Scheduling test lead")
                        }
                    }
                    Skin.TextField {
                        Layout.preferredWidth: 80
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
                    Item {
                        Layout.fillWidth: true
                    }
                    Skin.FormButton {
                        text: qsTr("Schedule play")

                        onClicked: root.schedulePlayTest()
                    }
                }

                Text {
                    Layout.fillWidth: true
                    color: Mixxx.Collab.scheduledCommandValid ? Theme.white : Theme.midGray
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    text: Mixxx.Collab.scheduledCommandValid ? qsTr("Deck %1 started %2 ms after the target").arg(Mixxx.Collab.lastCommandDeck).arg(Mixxx.Collab.lastCommandLatenessMs.toFixed(2)) : qsTr("The play command is aligned to the next two-second session-time boundary.")
                    wrapMode: Text.WordWrap
                }

                Hint {
                    text: qsTr("The command stops the deck first, then sets play at the target. Record both outputs on one device to measure the actual audio onset.")
                }
            }

            SectionTitle {
                text: qsTr("Diagnostics")
            }
            Panel {
                RowLayout {
                    spacing: 8

                    FieldLabel {
                        text: qsTr("CSV log")

                        Mixxx.SettingParameter {
                            label: qsTr("Diagnostics CSV path")
                        }
                    }
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

                Hint {
                    text: qsTr("Clock samples and scheduled commands are appended to this file. Summarize it with tools/collab_diagnostics_report.py.")
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
