#include "qml/qmlcollabproxy.h"

#include <QDir>
#include <QTimer>
#include <QVariant>
#include <algorithm>

#include "collab/collabprobe.h"
#include "control/controlproxy.h"
#include "moc_qmlcollabproxy.cpp"
#include "preferences/configobject.h"

namespace mixxx {
namespace qml {

namespace {

constexpr char kCollabGroup[] = "[Collab]";
constexpr char kPeerHostKey[] = "probePeerHost";
constexpr char kPeerPortKey[] = "probePeerPort";
constexpr char kLocalPortKey[] = "probeLocalPort";
constexpr char kIntervalKey[] = "probeIntervalMs";
constexpr char kDiagnosticsPathKey[] = "diagnosticsPath";
constexpr char kHostRoleKey[] = "hostRole";

/// Scheduled play targets are aligned to this grid so both peers can compute
/// the same target without a session handshake.
constexpr qint64 kPlayTestGridUs = 2 * 1000 * 1000;

constexpr int kDefaultPeerPort = 40410;
constexpr int kDefaultLocalPort = 40411;
constexpr int kDefaultIntervalMs = 200;

ConfigKey collabKey(const char* item) {
    return ConfigKey(QString::fromLatin1(kCollabGroup),
            QString::fromLatin1(item));
}

} // namespace

QmlCollabProxy::QmlCollabProxy(QObject* pParent)
        : QObject(pParent),
          m_pProbe(new collab::CollabProbe(this)) {
    loadSettings();

    connect(m_pProbe,
            &collab::CollabProbe::statisticsChanged,
            this,
            &QmlCollabProxy::statisticsChanged);
    connect(m_pProbe,
            &collab::CollabProbe::probeError,
            this,
            &QmlCollabProxy::setLastError);

    m_pSchedulerTimer = new QTimer(this);
    m_pSchedulerTimer->setInterval(5);
    m_pSchedulerTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pSchedulerTimer, &QTimer::timeout, this, &QmlCollabProxy::tickScheduler);
}

QmlCollabProxy::~QmlCollabProxy() = default;

bool QmlCollabProxy::probeRunning() const {
    return m_pProbe->isRunning();
}

QString QmlCollabProxy::peerHost() const {
    return m_peerHost;
}

void QmlCollabProxy::setPeerHost(const QString& peerHost) {
    if (m_peerHost == peerHost) {
        return;
    }
    m_peerHost = peerHost;
    storeValue(kPeerHostKey, m_peerHost);
    emit peerHostChanged();
}

int QmlCollabProxy::peerPort() const {
    return m_peerPort;
}

void QmlCollabProxy::setPeerPort(int peerPort) {
    if (m_peerPort == peerPort) {
        return;
    }
    m_peerPort = peerPort;
    storeValue(kPeerPortKey, m_peerPort);
    emit peerPortChanged();
}

int QmlCollabProxy::localPort() const {
    return m_localPort;
}

void QmlCollabProxy::setLocalPort(int localPort) {
    if (m_localPort == localPort) {
        return;
    }
    m_localPort = localPort;
    storeValue(kLocalPortKey, m_localPort);
    emit localPortChanged();
}

int QmlCollabProxy::intervalMs() const {
    return m_intervalMs;
}

void QmlCollabProxy::setIntervalMs(int intervalMs) {
    if (m_intervalMs == intervalMs) {
        return;
    }
    m_intervalMs = intervalMs;
    storeValue(kIntervalKey, m_intervalMs);
    emit intervalMsChanged();

    // Restart to apply the new interval immediately.
    if (m_pProbe->isRunning()) {
        m_pProbe->stop();
        emit probeRunningChanged();
        startProbe();
    }
}

QString QmlCollabProxy::diagnosticsPath() const {
    return m_diagnosticsPath;
}

void QmlCollabProxy::setDiagnosticsPath(const QString& path) {
    if (m_diagnosticsPath == path) {
        return;
    }
    m_diagnosticsPath = path;
    storeValue(kDiagnosticsPathKey, m_diagnosticsPath);
    emit diagnosticsPathChanged();
}

double QmlCollabProxy::rttMs() const {
    return static_cast<double>(m_pProbe->rttUs()) / 1000.0;
}

double QmlCollabProxy::offsetMs() const {
    return static_cast<double>(m_pProbe->offsetUs()) / 1000.0;
}

double QmlCollabProxy::jitterMs() const {
    return static_cast<double>(m_pProbe->jitterUs()) / 1000.0;
}

double QmlCollabProxy::skewPpm() const {
    return m_pProbe->skewPpm();
}

int QmlCollabProxy::sampleCount() const {
    return m_pProbe->sampleCount();
}

QString QmlCollabProxy::lastError() const {
    return m_lastError;
}

bool QmlCollabProxy::startProbe() {
    setLastError(QString());

    QString error;
    if (!m_pProbe->diagnostics()->open(m_diagnosticsPath, &error)) {
        setLastError(tr("Cannot open diagnostics CSV: %1").arg(error));
        // The probe is still useful without logging.
    }

    if (!m_pProbe->start(m_peerHost,
                static_cast<quint16>(m_peerPort),
                static_cast<quint16>(m_localPort),
                m_intervalMs)) {
        setLastError(m_pProbe->errorString());
        emit probeRunningChanged();
        return false;
    }
    emit probeRunningChanged();
    emit statisticsChanged();
    return true;
}

void QmlCollabProxy::stopProbe() {
    m_pProbe->stop();
    m_pProbe->diagnostics()->close();
    emit probeRunningChanged();
}

void QmlCollabProxy::clearStatistics() {
    m_pProbe->clearStatistics();
}

bool QmlCollabProxy::hostRole() const {
    return m_hostRole;
}

void QmlCollabProxy::setHostRole(bool hostRole) {
    if (m_hostRole == hostRole) {
        return;
    }
    m_hostRole = hostRole;
    storeValue(kHostRoleKey, m_hostRole ? QStringLiteral("1") : QStringLiteral("0"));
    emit hostRoleChanged();
}

bool QmlCollabProxy::scheduledCommandValid() const {
    return m_scheduledCommandValid;
}

double QmlCollabProxy::lastCommandLatenessMs() const {
    return m_lastCommandLatenessMs;
}

int QmlCollabProxy::lastCommandDeck() const {
    return m_lastCommandDeck;
}

qint64 QmlCollabProxy::sessionNowUs() const {
    const qint64 localUs = m_pProbe->nowUs();
    return m_hostRole ? localUs : m_pProbe->clockSync().toRemoteTime(localUs);
}

bool QmlCollabProxy::schedulePlayTest(int deck, int leadMs) {
    if (deck < 1 || deck > 4) {
        setLastError(tr("Deck must be between 1 and 4"));
        return false;
    }
    if (!m_hostRole && !m_pProbe->clockSync().hasEstimate()) {
        setLastError(tr("Start the clock probe before scheduling a play test"));
        return false;
    }

    const qint64 nowUs = sessionNowUs();
    const qint64 requestedUs = nowUs + static_cast<qint64>(std::max(leadMs, 200)) * 1000;
    // Next grid boundary strictly after the requested time.
    const qint64 targetUs = ((requestedUs / kPlayTestGridUs) + 1) * kPlayTestGridUs;

    m_pPlayProxy = std::make_unique<ControlProxy>(
            ConfigKey(QStringLiteral("[Channel%1]").arg(deck),
                    QStringLiteral("play")),
            this);
    if (!m_pPlayProxy->valid()) {
        setLastError(tr("Deck %1 is not available").arg(deck));
        return false;
    }
    // Start from a stopped deck so the test measures the play transition.
    m_pPlayProxy->set(0.0);

    const quint64 commandId = m_nextCommandId++;
    m_lastCommandDeck = deck;
    m_lastCommandLatenessMs = 0.0;
    m_scheduledCommandValid = false;

    const bool scheduled = m_scheduler.schedule(
            commandId,
            targetUs,
            [this, commandId, targetUs](collab::CollabCommandScheduler::CommandId,
                    qint64 latenessUs) {
                if (m_pPlayProxy) {
                    m_pPlayProxy->set(1.0);
                }
                m_lastCommandLatenessMs = static_cast<double>(latenessUs) / 1000.0;
                m_scheduledCommandValid = true;
                if (m_pProbe->diagnostics()->isOpen()) {
                    m_pProbe->diagnostics()->logScheduledCommand(commandId,
                            targetUs,
                            targetUs + latenessUs,
                            latenessUs,
                            false);
                }
                emit lastCommandChanged();
            });
    if (!scheduled) {
        setLastError(tr("Could not schedule the play test"));
        return false;
    }

    m_pSchedulerTimer->start();
    emit lastCommandChanged();
    return true;
}

void QmlCollabProxy::cancelScheduledCommands() {
    m_scheduler.clear();
    if (m_pSchedulerTimer) {
        m_pSchedulerTimer->stop();
    }
    m_scheduledCommandValid = false;
    emit lastCommandChanged();
}

void QmlCollabProxy::tickScheduler() {
    m_scheduler.runDue(sessionNowUs());
    if (m_scheduler.pendingCount() == 0) {
        m_pSchedulerTimer->stop();
    }
}

QString QmlCollabProxy::defaultDiagnosticsPath() const {
    if (!s_pConfig) {
        return QString();
    }
    return QDir(s_pConfig->getSettingsPath())
            .filePath(QStringLiteral("collab-diagnostics.csv"));
}

void QmlCollabProxy::loadSettings() {
    m_peerHost = loadValue(kPeerHostKey, QStringLiteral("127.0.0.1")).toString();
    m_peerPort = loadValue(kPeerPortKey, kDefaultPeerPort).toInt();
    m_localPort = loadValue(kLocalPortKey, kDefaultLocalPort).toInt();
    m_intervalMs = loadValue(kIntervalKey, kDefaultIntervalMs).toInt();
    if (m_intervalMs <= 0) {
        m_intervalMs = kDefaultIntervalMs;
    }
    m_diagnosticsPath = loadValue(kDiagnosticsPathKey, QString()).toString();
    if (m_diagnosticsPath.isEmpty()) {
        m_diagnosticsPath = defaultDiagnosticsPath();
    }
    m_hostRole = loadValue(kHostRoleKey, QStringLiteral("1")).toString() != QStringLiteral("0");
}

void QmlCollabProxy::storeValue(const QString& key, const QVariant& value) {
    if (!s_pConfig) {
        return;
    }
    s_pConfig->setValue(collabKey(key.toLatin1().constData()),
            value.toString());
}

QVariant QmlCollabProxy::loadValue(const QString& key,
        const QVariant& defaultValue) const {
    if (!s_pConfig) {
        return defaultValue;
    }
    return s_pConfig->getValue(collabKey(key.toLatin1().constData()),
            defaultValue.toString());
}

void QmlCollabProxy::setLastError(const QString& error) {
    if (m_lastError == error) {
        return;
    }
    m_lastError = error;
    emit lastErrorChanged();
}

} // namespace qml
} // namespace mixxx
