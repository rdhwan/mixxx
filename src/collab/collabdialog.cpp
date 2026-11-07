#include "collab/collabdialog.h"

#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "collab/collabconnection.h"
#include "collab/collabprotocol.h"
#include "moc_collabdialog.cpp"
#include "preferences/configobject.h"

namespace mixxx {
namespace collab {

namespace {

constexpr char kCollabGroup[] = "[Collab]";
constexpr char kPeerHostKey[] = "peerHost";
constexpr char kHostRoleKey[] = "hostRole";
constexpr char kSessionCodeKey[] = "sessionCode";
constexpr char kClockIntervalKey[] = "clockIntervalMs";
constexpr char kDiagnosticsPathKey[] = "diagnosticsPath";

constexpr quint16 kHostLocalPort = 40410;
constexpr quint16 kGuestLocalPort = 40411;
constexpr int kDefaultClockIntervalMs = 200;

ConfigKey collabKey(const char* item) {
    return ConfigKey(QString::fromLatin1(kCollabGroup),
            QString::fromLatin1(item));
}

QString formatMs(qint64 microseconds) {
    return QString::number(static_cast<double>(microseconds) / 1000.0, 'f', 2) +
            QStringLiteral(" ms");
}

QLabel* makeMetricValue() {
    auto* pLabel = new QLabel(QStringLiteral("—"));
    QFont font = pLabel->font();
    font.setBold(true);
    pLabel->setFont(font);
    return pLabel;
}

QFrame* makeSwatch() {
    auto* pSwatch = new QFrame;
    pSwatch->setFixedSize(12, 12);
    pSwatch->setFrameShape(QFrame::NoFrame);
    return pSwatch;
}

} // namespace

CollabDialog::CollabDialog(QWidget* pParent, UserSettingsPointer pConfig)
        : QDialog(pParent),
          m_pConnection(CollabConnection::shared()),
          m_pConfig(std::move(pConfig)) {
    setWindowTitle(tr("Online B2B"));
    setMinimumWidth(560);

    buildUi();
    loadSettings();
    applyRole();

    connect(m_pConnection,
            &CollabConnection::stateChanged,
            this,
            &CollabDialog::refreshState);
    connect(m_pConnection,
            &CollabConnection::statisticsChanged,
            this,
            &CollabDialog::refreshMetrics);
    connect(m_pConnection,
            &CollabConnection::errorOccurred,
            this,
            [this](const QString& message) {
                m_pErrorLabel->setText(message);
                m_pErrorLabel->setVisible(!message.isEmpty());
                refreshState();
            });
    connect(m_pConnection,
            &CollabConnection::playApplied,
            this,
            &CollabDialog::onPlayApplied);
    connect(m_pConnection,
            &CollabConnection::localInputBlocked,
            this,
            [this](int, const QString&) { refreshMetrics(); });
    connect(m_pConnection,
            &CollabConnection::trackSharingChanged,
            this,
            &CollabDialog::refreshTrackSharing);
    connect(m_pConnection,
            &CollabConnection::trackLoadRequested,
            this,
            &CollabDialog::refreshTrackSharing);
    connect(m_pConnection,
            &CollabConnection::trackLoadCompleted,
            this,
            [this](int, const CollabContentHash&) {
                m_transferReceived = 0;
                m_transferTotal = 0;
                refreshTrackSharing();
            });
    connect(m_pConnection,
            &CollabConnection::trackLoadFailed,
            this,
            [this](int, const CollabContentHash&, const QString&) {
                m_transferReceived = 0;
                m_transferTotal = 0;
                refreshTrackSharing();
            });
    connect(m_pConnection,
            &CollabConnection::trackTransferProgress,
            this,
            [this](const CollabContentHash&, qint64 received, qint64 total) {
                m_transferReceived = received;
                m_transferTotal = total;
                refreshTrackSharing();
            });

    refreshState();
}

void CollabDialog::buildUi() {
    auto* pMainLayout = new QVBoxLayout(this);

    // Connection
    auto* pConnectionBox = new QGroupBox(tr("Connection"), this);
    auto* pConnectionLayout = new QVBoxLayout(pConnectionBox);

    auto* pStatusRow = new QHBoxLayout;
    m_pStatusSwatch = makeSwatch();
    m_pStatusLabel = new QLabel(this);
    QFont statusFont = m_pStatusLabel->font();
    statusFont.setBold(true);
    m_pStatusLabel->setFont(statusFont);
    m_pConnectButton = new QPushButton(tr("Connect"), this);
    pStatusRow->addWidget(m_pStatusSwatch);
    pStatusRow->addWidget(m_pStatusLabel, 1);
    pStatusRow->addWidget(m_pConnectButton);
    pConnectionLayout->addLayout(pStatusRow);

    auto* pForm = new QFormLayout;
    m_pRoleCombo = new QComboBox(this);
    m_pRoleCombo->addItem(tr("Host"), true);
    m_pRoleCombo->addItem(tr("Guest"), false);
    pForm->addRow(tr("Session role"), m_pRoleCombo);

    m_pPeerHostEdit = new QLineEdit(this);
    m_pPeerHostEdit->setPlaceholderText(QStringLiteral("127.0.0.1"));
    pForm->addRow(tr("Host address"), m_pPeerHostEdit);

    auto* pCodeRow = new QHBoxLayout;
    m_pSessionCodeEdit = new QLineEdit(this);
    m_pSessionCodeEdit->setMaxLength(16);
    m_pSessionCodeEdit->setPlaceholderText(tr("shared code"));
    QFont codeFont = m_pSessionCodeEdit->font();
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    m_pSessionCodeEdit->setFont(codeFont);
    m_pNewCodeButton = new QPushButton(tr("New"), this);
    pCodeRow->addWidget(m_pSessionCodeEdit, 1);
    pCodeRow->addWidget(m_pNewCodeButton);
    pForm->addRow(tr("Session code"), pCodeRow);

    m_pPortsLabel = new QLabel(this);
    pForm->addRow(tr("UDP ports"), m_pPortsLabel);
    pConnectionLayout->addLayout(pForm);

    auto* pHint = new QLabel(tr("Both peers enter the same session code. The host "
                                "shows it, the guest types it. Ports follow the "
                                "role: host 40410, guest 40411."),
            this);
    pHint->setWordWrap(true);
    pHint->setEnabled(false);
    pConnectionLayout->addWidget(pHint);
    pMainLayout->addWidget(pConnectionBox);

    // Clock
    auto* pClockBox = new QGroupBox(tr("Clock"), this);
    auto* pClockLayout = new QVBoxLayout(pClockBox);
    m_pClockStatusLabel = new QLabel(this);
    pClockLayout->addWidget(m_pClockStatusLabel);

    auto* pMetrics = new QGridLayout;
    const QStringList metricNames = {tr("Round trip"),
            tr("Offset"),
            tr("Jitter"),
            tr("Skew"),
            tr("Samples"),
            tr("Drift"),
            tr("Packet loss"),
            tr("Cmd drops")};
    QLabel** pValues[] = {&m_pRttValue,
            &m_pOffsetValue,
            &m_pJitterValue,
            &m_pSkewValue,
            &m_pSamplesValue,
            &m_pDriftValue,
            &m_pPacketLossValue,
            &m_pDroppedCommandsValue};
    constexpr int kMetricsPerRow = 4;
    for (int i = 0; i < metricNames.size(); ++i) {
        const int row = (i / kMetricsPerRow) * 2;
        const int column = i % kMetricsPerRow;
        pMetrics->addWidget(new QLabel(metricNames[i], this), row, column);
        *pValues[i] = makeMetricValue();
        pMetrics->addWidget(*pValues[i], row + 1, column);
    }
    pClockLayout->addLayout(pMetrics);
    pMainLayout->addWidget(pClockBox);

    // Track sharing
    auto* pSharingBox = new QGroupBox(tr("Track sharing"), this);
    auto* pSharingLayout = new QVBoxLayout(pSharingBox);
    m_pSharingStatusLabel = new QLabel(this);
    m_pSharingStatusLabel->setWordWrap(true);
    pSharingLayout->addWidget(m_pSharingStatusLabel);
    m_pSharingActivityLabel = new QLabel(this);
    m_pSharingActivityLabel->setWordWrap(true);
    pSharingLayout->addWidget(m_pSharingActivityLabel);
    pMainLayout->addWidget(pSharingBox);

    // Scheduling test
    auto* pScheduleBox = new QGroupBox(tr("Scheduling test"), this);
    auto* pScheduleLayout = new QVBoxLayout(pScheduleBox);
    auto* pScheduleRow = new QHBoxLayout;
    pScheduleRow->addWidget(new QLabel(tr("Deck"), this));
    m_pDeckCombo = new QComboBox(this);
    pScheduleRow->addWidget(m_pDeckCombo);
    pScheduleRow->addWidget(new QLabel(tr("Lead"), this));
    m_pLeadSpin = new QSpinBox(this);
    m_pLeadSpin->setRange(200, 20000);
    m_pLeadSpin->setSingleStep(100);
    m_pLeadSpin->setValue(3000);
    m_pLeadSpin->setSuffix(tr(" ms"));
    pScheduleRow->addWidget(m_pLeadSpin);
    m_pScheduleButton = new QPushButton(tr("Schedule play"), this);
    pScheduleRow->addWidget(m_pScheduleButton);
    pScheduleRow->addStretch(1);
    pScheduleLayout->addLayout(pScheduleRow);
    m_pScheduleResultLabel = new QLabel(tr("Connect both peers first."), this);
    m_pScheduleResultLabel->setWordWrap(true);
    pScheduleLayout->addWidget(m_pScheduleResultLabel);
    pMainLayout->addWidget(pScheduleBox);

    // Diagnostics
    auto* pDiagnosticsBox = new QGroupBox(tr("Diagnostics"), this);
    auto* pDiagnosticsLayout = new QHBoxLayout(pDiagnosticsBox);
    pDiagnosticsLayout->addWidget(new QLabel(tr("CSV log"), this));
    m_pDiagnosticsPathEdit = new QLineEdit(this);
    pDiagnosticsLayout->addWidget(m_pDiagnosticsPathEdit, 1);
    m_pDefaultDiagnosticsButton = new QPushButton(tr("Default"), this);
    pDiagnosticsLayout->addWidget(m_pDefaultDiagnosticsButton);
    pMainLayout->addWidget(pDiagnosticsBox);

    m_pErrorLabel = new QLabel(this);
    m_pErrorLabel->setWordWrap(true);
    QPalette errorPalette = m_pErrorLabel->palette();
    errorPalette.setColor(QPalette::WindowText, Qt::red);
    m_pErrorLabel->setPalette(errorPalette);
    m_pErrorLabel->hide();
    pMainLayout->addWidget(m_pErrorLabel);

    m_pOwnershipLabel = new QLabel(this);
    m_pOwnershipLabel->setWordWrap(true);
    QPalette ownershipPalette = m_pOwnershipLabel->palette();
    ownershipPalette.setColor(QPalette::WindowText, QColor(0xB8, 0x86, 0x00));
    m_pOwnershipLabel->setPalette(ownershipPalette);
    m_pOwnershipLabel->hide();
    pMainLayout->addWidget(m_pOwnershipLabel);

    pMainLayout->addStretch(1);

    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::close);
    pMainLayout->addWidget(pButtons);

    connect(m_pConnectButton, &QPushButton::clicked, this, &CollabDialog::onConnectClicked);
    connect(m_pRoleCombo,
            &QComboBox::currentIndexChanged,
            this,
            &CollabDialog::applyRole);
    connect(m_pPeerHostEdit, &QLineEdit::editingFinished, this, &CollabDialog::saveSettings);
    connect(m_pSessionCodeEdit,
            &QLineEdit::editingFinished,
            this,
            &CollabDialog::saveSettings);
    connect(m_pNewCodeButton, &QPushButton::clicked, this, &CollabDialog::onNewCodeClicked);
    connect(m_pScheduleButton, &QPushButton::clicked, this, &CollabDialog::onScheduleClicked);
    connect(m_pDiagnosticsPathEdit,
            &QLineEdit::editingFinished,
            this,
            &CollabDialog::saveSettings);
    connect(m_pDefaultDiagnosticsButton,
            &QPushButton::clicked,
            this,
            &CollabDialog::onDefaultDiagnosticsClicked);
}

void CollabDialog::loadSettings() {
    if (!m_pConfig) {
        return;
    }
    m_hostRole = m_pConfig->getValue<QString>(collabKey(kHostRoleKey),
                         QStringLiteral("1")) != QStringLiteral("0");
    m_pRoleCombo->setCurrentIndex(m_hostRole ? 0 : 1);

    m_pPeerHostEdit->setText(m_pConfig->getValue<QString>(
            collabKey(kPeerHostKey), QStringLiteral("127.0.0.1")));

    m_pSessionCodeEdit->setText(
            m_pConfig->getValue<QString>(collabKey(kSessionCodeKey), QString())
                    .trimmed()
                    .toUpper());

    m_clockIntervalMs = m_pConfig->getValue<QString>(
                                         collabKey(kClockIntervalKey),
                                         QString::number(kDefaultClockIntervalMs))
                                .toInt();
    if (m_clockIntervalMs <= 0) {
        m_clockIntervalMs = kDefaultClockIntervalMs;
    }
    m_pConnection->setClockIntervalMs(m_clockIntervalMs);

    m_pDiagnosticsPathEdit->setText(m_pConfig->getValue<QString>(
            collabKey(kDiagnosticsPathKey), QString()));
}

void CollabDialog::saveSettings() {
    if (!m_pConfig) {
        return;
    }
    m_pConfig->setValue(collabKey(kHostRoleKey),
            m_hostRole ? QStringLiteral("1") : QStringLiteral("0"));
    m_pConfig->setValue(collabKey(kPeerHostKey), m_pPeerHostEdit->text().trimmed());
    m_pConfig->setValue(
            collabKey(kSessionCodeKey), m_pSessionCodeEdit->text().trimmed().toUpper());
    m_pConfig->setValue(collabKey(kDiagnosticsPathKey), m_pDiagnosticsPathEdit->text());
    m_pConfig->save();
}

void CollabDialog::applyRole() {
    m_hostRole = m_pRoleCombo->currentData().toBool();
    m_pSessionCodeEdit->setEnabled(!isSessionActive());
    m_pNewCodeButton->setEnabled(m_hostRole && !isSessionActive());
    m_pPeerHostEdit->setEnabled(!m_hostRole && !isSessionActive());

    m_pDeckCombo->clear();
    if (m_hostRole) {
        m_pDeckCombo->addItem(tr("Deck 1"), 1);
        m_pDeckCombo->addItem(tr("Deck 2"), 2);
    } else {
        m_pDeckCombo->addItem(tr("Deck 3"), 3);
        m_pDeckCombo->addItem(tr("Deck 4"), 4);
    }
    updatePortsLabel();
    saveSettings();
}

void CollabDialog::updatePortsLabel() {
    const quint16 local = localPort();
    const quint16 peer = peerPort();
    m_pPortsLabel->setText(tr("local %1 → peer %2").arg(local).arg(peer));
}

bool CollabDialog::isSessionActive() const {
    return m_pConnection->isActive();
}

quint16 CollabDialog::localPort() const {
    return m_hostRole ? kHostLocalPort : kGuestLocalPort;
}

quint16 CollabDialog::peerPort() const {
    return m_hostRole ? kGuestLocalPort : kHostLocalPort;
}

QString CollabDialog::effectiveDiagnosticsPath() const {
    if (!m_pDiagnosticsPathEdit->text().trimmed().isEmpty()) {
        return m_pDiagnosticsPathEdit->text().trimmed();
    }
    if (!m_pConfig) {
        return QString();
    }
    return QDir(m_pConfig->getSettingsPath())
            .filePath(QStringLiteral("collab-diagnostics.csv"));
}

void CollabDialog::onConnectClicked() {
    if (isSessionActive()) {
        m_pConnection->leave();
        refreshState();
        return;
    }

    saveSettings();
    m_pErrorLabel->clear();
    m_pErrorLabel->hide();

    QString code = m_pSessionCodeEdit->text().trimmed().toUpper();
    if (code.isEmpty()) {
        if (!m_hostRole) {
            m_pErrorLabel->setText(tr("Enter the session code shown by the host."));
            m_pErrorLabel->show();
            return;
        }
        code = makeCollabSessionCode();
        m_pSessionCodeEdit->setText(code);
        saveSettings();
    }

    const CollabSessionToken token = collabTokenFromCode(code);
    const QString diagnosticsPath = effectiveDiagnosticsPath();
    const bool started = m_hostRole
            ? m_pConnection->host(localPort(), token, diagnosticsPath)
            : m_pConnection->join(m_pPeerHostEdit->text().trimmed(),
                      peerPort(),
                      localPort(),
                      token,
                      diagnosticsPath);
    if (!started) {
        m_pErrorLabel->setText(m_pConnection->errorString());
        m_pErrorLabel->show();
    }
    refreshState();
}

void CollabDialog::onNewCodeClicked() {
    m_pSessionCodeEdit->setText(makeCollabSessionCode());
    saveSettings();
}

void CollabDialog::onScheduleClicked() {
    const int deck = m_pDeckCombo->currentData().toInt();
    m_pScheduleButton->setEnabled(false);
    const bool scheduled =
            m_pConnection->schedulePlay(deck, m_pLeadSpin->value());
    m_pScheduleButton->setEnabled(m_pConnection->isConnected());
    if (!scheduled) {
        m_pScheduleResultLabel->setText(m_pConnection->errorString());
        return;
    }
    m_pScheduleResultLabel->setText(tr("Deck %1 scheduled.").arg(deck));
}

void CollabDialog::onDefaultDiagnosticsClicked() {
    m_pDiagnosticsPathEdit->clear();
    saveSettings();
}

void CollabDialog::onPlayApplied(int deck, double latenessMs, bool remote) {
    m_pScheduleResultLabel->setText(
            remote ? tr("Deck %1 started %2 ms after the target (remote command).")
                             .arg(deck)
                             .arg(QString::number(latenessMs, 'f', 2))
                   : tr("Deck %1 started %2 ms after the target; the command was sent "
                        "to the peer.")
                             .arg(deck)
                             .arg(QString::number(latenessMs, 'f', 2)));
}

void CollabDialog::refreshState() {
    const CollabSession::State state = m_pConnection->state();
    const bool active = isSessionActive();
    const bool connected = m_pConnection->isConnected();

    m_pRoleCombo->setEnabled(!active);
    m_pPeerHostEdit->setEnabled(!active && !m_hostRole);
    m_pSessionCodeEdit->setEnabled(!active);
    m_pNewCodeButton->setEnabled(!active && m_hostRole);
    m_pDiagnosticsPathEdit->setEnabled(!active);
    m_pDefaultDiagnosticsButton->setEnabled(!active);
    m_pConnectButton->setText(active ? tr("Disconnect") : tr("Connect"));

    m_pScheduleButton->setEnabled(connected);
    m_pDeckCombo->setEnabled(connected);
    m_pLeadSpin->setEnabled(connected);

    QString status;
    QString color;
    if (m_pConnection->reconnecting()) {
        status = tr("Reconnecting to the peer…");
        color = QStringLiteral("#d0a000");
    } else {
        switch (state) {
        case CollabSession::State::Idle:
            status = tr("Not connected");
            color = QStringLiteral("#808080");
            break;
        case CollabSession::State::Listening:
            status = tr("Waiting for a guest on port %1…").arg(localPort());
            color = QStringLiteral("#d0a000");
            break;
        case CollabSession::State::Joining:
            status = tr("Joining %1:%2…")
                             .arg(m_pPeerHostEdit->text().trimmed())
                             .arg(peerPort());
            color = QStringLiteral("#d0a000");
            break;
        case CollabSession::State::Connected:
            status = tr("Connected to %1").arg(m_pConnection->peerAddress());
            color = QStringLiteral("#2e9e44");
            break;
        case CollabSession::State::Closed:
            status = tr("Disconnected");
            color = QStringLiteral("#c03030");
            break;
        }
    }
    m_pStatusLabel->setText(status);
    m_pStatusSwatch->setStyleSheet(
            QStringLiteral("background-color: %1; border-radius: 6px;").arg(color));

    if (m_pConnection->remoteDecksStale()) {
        m_pErrorLabel->setText(
                tr("Connection lost. Remote decks keep their last state until "
                   "the peer returns."));
        m_pErrorLabel->show();
    } else if (state == CollabSession::State::Closed &&
            !m_pConnection->errorString().isEmpty()) {
        m_pErrorLabel->setText(m_pConnection->errorString());
        m_pErrorLabel->show();
    } else if (m_pErrorLabel->text().isEmpty()) {
        m_pErrorLabel->hide();
    }

    refreshMetrics();
}

void CollabDialog::refreshMetrics() {
    m_pRttValue->setText(formatMs(m_pConnection->rttUs()));
    m_pOffsetValue->setText(formatMs(m_pConnection->offsetUs()));
    m_pJitterValue->setText(formatMs(m_pConnection->jitterUs()));
    m_pSkewValue->setText(
            QString::number(m_pConnection->skewPpm(), 'f', 2) + QStringLiteral(" ppm"));
    m_pSamplesValue->setText(QString::number(m_pConnection->sampleCount()));
    m_pDriftValue->setText(
            QString::number(m_pConnection->lastRemoteDriftMs(), 'f', 1) +
            QStringLiteral(" ms"));
    m_pPacketLossValue->setText(
            QString::number(m_pConnection->packetLossPercent(), 'f', 1) +
            QStringLiteral(" %"));
    m_pDroppedCommandsValue->setText(
            QString::number(m_pConnection->droppedCommandCount()));
    m_pDroppedCommandsValue->setToolTip(
            tr("%1 commands executed, worst lateness %2 ms")
                    .arg(m_pConnection->executedCommandCount())
                    .arg(QString::number(
                            m_pConnection->maxCommandLatenessMs(), 'f', 2)));

    if (!m_pConnection->isConnected()) {
        m_pClockStatusLabel->setText(tr("No peer."));
    } else if (m_pConnection->hasClockEstimate()) {
        m_pClockStatusLabel->setText(tr("Clock synchronised."));
    } else {
        m_pClockStatusLabel->setText(tr("Waiting for the clock estimate…"));
    }

    const QString ownership = m_pConnection->ownershipNotice();
    m_pOwnershipLabel->setText(ownership);
    m_pOwnershipLabel->setVisible(!ownership.isEmpty());

    refreshTrackSharing();
}

void CollabDialog::refreshTrackSharing() {
    if (!m_pConnection->isConnected()) {
        m_pSharingStatusLabel->setText(
                tr("Track sharing needs a connected session."));
        m_pSharingActivityLabel->clear();
        m_pSharingActivityLabel->hide();
        return;
    }
    if (!m_pConnection->isTrackSharingEnabled()) {
        m_pSharingStatusLabel->setText(tr("Track sharing is off."));
        m_pSharingActivityLabel->clear();
        m_pSharingActivityLabel->hide();
        return;
    }
    m_pSharingStatusLabel->setText(
            tr("Sharing is active on transfer port %1.")
                    .arg(m_pConnection->trackTransferPort()));

    QStringList parts;
    for (int deck = 1; deck <= kCollabSessionDeckCount; ++deck) {
        switch (m_pConnection->trackLoadState(deck)) {
        case CollabConnection::TrackLoadState::WaitingForPeer:
            parts << tr("Deck %1: waiting for the peer to load the track")
                             .arg(deck);
            break;
        case CollabConnection::TrackLoadState::LoadingSharedTrack:
            parts << tr("Deck %1: loading the shared track").arg(deck);
            break;
        case CollabConnection::TrackLoadState::Idle:
            break;
        }
    }
    if (m_transferTotal > 0) {
        parts << tr("Receiving track: %L1 %")
                         .arg(static_cast<int>(
                                 qBound<qint64>(0,
                                         m_transferReceived * 100,
                                         m_transferTotal * 100) /
                                 m_transferTotal));
    }
    m_pSharingActivityLabel->setText(parts.join(QStringLiteral(" · ")));
    m_pSharingActivityLabel->setVisible(!parts.isEmpty());
}

} // namespace collab
} // namespace mixxx
