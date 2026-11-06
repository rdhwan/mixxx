import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Mixxx 1.0 as Mixxx
import "Settings" as Settings
import "Theme"

/// Shared content of the Online B2B diagnostics. Used by the popup in QML
/// skins and by the standalone window that legacy (QWidget) skins open.
Item {
    id: root

    signal closeRequested

    ColumnLayout {
        anchors.fill: parent
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

                onClicked: root.closeRequested()
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
