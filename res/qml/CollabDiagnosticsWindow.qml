import QtQuick
import QtQuick.Controls
import "Theme"

/// Standalone diagnostics window for skins that do not embed the full QML
/// settings popup. It reuses the Online B2B settings page and is opened from the
/// Developer menu. Legacy (QWidget) skins load this file in their own QML
/// engine.
Window {
    id: root

    color: Theme.darkGray2
    height: 860
    minimumHeight: 640
    minimumWidth: 620
    title: qsTr("Online B2B diagnostics")
    visible: false
    width: 780

    CollabDiagnosticsPanel {
        anchors.fill: parent
        anchors.margins: 18

        onCloseRequested: root.close()
    }
}
