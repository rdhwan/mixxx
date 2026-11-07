#pragma once

#include <QDialog>

#include "preferences/usersettings.h"

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace mixxx {
namespace collab {

class CollabConnection;

/// Native dialog for the Online B2B session. Legacy (QWidget) skins use this
/// window from the Developer menu; QML skins use their settings page instead.
///
/// The session itself lives in CollabConnection::shared(), so closing this
/// window does not end a running session. Reopening it shows the live state.
class CollabDialog : public QDialog {
    Q_OBJECT
  public:
    CollabDialog(QWidget* pParent, UserSettingsPointer pConfig);

  private:
    void buildUi();
    void loadSettings();
    void saveSettings();

    void refreshState();
    void refreshMetrics();
    void applyRole();
    void updatePortsLabel();

    void onConnectClicked();
    void onNewCodeClicked();
    void onScheduleClicked();
    void onDefaultDiagnosticsClicked();
    void onPlayApplied(int deck, double latenessMs, bool remote);

    bool isSessionActive() const;
    quint16 localPort() const;
    quint16 peerPort() const;
    QString effectiveDiagnosticsPath() const;

    CollabConnection* m_pConnection;
    UserSettingsPointer m_pConfig;

    QComboBox* m_pRoleCombo;
    QLineEdit* m_pPeerHostEdit;
    QLineEdit* m_pSessionCodeEdit;
    QPushButton* m_pNewCodeButton;
    QPushButton* m_pConnectButton;
    QFrame* m_pStatusSwatch;
    QLabel* m_pStatusLabel;
    QLabel* m_pPortsLabel;

    QLabel* m_pClockStatusLabel;
    QLabel* m_pRttValue;
    QLabel* m_pOffsetValue;
    QLabel* m_pJitterValue;
    QLabel* m_pSkewValue;
    QLabel* m_pSamplesValue;

    QComboBox* m_pDeckCombo;
    QSpinBox* m_pLeadSpin;
    QPushButton* m_pScheduleButton;
    QLabel* m_pScheduleResultLabel;

    QLineEdit* m_pDiagnosticsPathEdit;
    QPushButton* m_pDefaultDiagnosticsButton;

    QLabel* m_pErrorLabel;
    QLabel* m_pOwnershipLabel;

    bool m_hostRole = true;
    int m_clockIntervalMs = 200;
};

} // namespace collab
} // namespace mixxx
