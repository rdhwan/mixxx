import QtQuick
import QtQuick.Controls
import "Theme"

/// Popup host for the Online B2B diagnostics in QML skins.
Popup {
    id: root

    anchors.centerIn: parent
    height: Math.min(820, parent ? parent.height - 80 : 820)
    modal: true
    padding: 18
    parent: Overlay.overlay
    width: Math.min(760, parent ? parent.width - 80 : 760)

    background: Rectangle {
        border.color: Theme.darkGray3
        border.width: 1
        color: Theme.darkGray2
        radius: 8
    }
    contentItem: CollabDiagnosticsPanel {
        onCloseRequested: root.close()
    }
}
