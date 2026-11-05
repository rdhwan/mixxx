import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mixxx 1.0 as Mixxx
import "Settings" as Settings
import "Theme"

/// Standalone diagnostics window for skins that do not embed the full QML
/// settings popup (LateNightQML). It reuses the Online B2B settings page and is
/// opened from the Developer menu.
Popup {
    id: root

    anchors.centerIn: parent
    height: Math.min(820, parent ? parent.height - 80 : 820)
    modal: true
    padding: 0
    parent: Overlay.overlay
    width: Math.min(760, parent ? parent.width - 80 : 760)

    background: Rectangle {
        border.color: Theme.darkGray3
        border.width: 1
        color: Theme.darkGray2
        radius: 8
    }
    contentItem: Item {
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    color: Theme.white
                    font.family: Theme.fontFamily
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    text: qsTr("Online B2B diagnostics")
                }
                Item {
                    Layout.fillWidth: true
                }
                Settings.FormButton {
                    text: qsTr("Close")

                    onClicked: root.close()
                }
            }
            Mixxx.SettingParameterManager {
                Layout.fillHeight: true
                Layout.fillWidth: true

                Settings.Collab {
                    anchors.fill: parent
                }
            }
        }
    }
}
