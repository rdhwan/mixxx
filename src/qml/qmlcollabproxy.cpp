#include "qml/qmlcollabproxy.h"

#include <QDir>
#include <QVariant>

#include "collab/collabconnection.h"
#include "collab/collabprotocol.h"
#include "moc_qmlcollabproxy.cpp"
#include "preferences/configobject.h"

namespace mixxx {
namespace qml {

namespace {

constexpr char kCollabGroup[] = "[Collab]";
constexpr char kPeerHostKey[] = "peerHost";
constexpr char kLocalPortKey[] = "localPort";
constexpr char kIntervalKey[] = "clockIntervalMs";
constexpr char kSessionCodeKey[] = "sessionCode";
constexpr char kDiagnosticsPathKey[] = "diagnosticsPath";
constexpr char kHostRoleKey[] = "hostRole";

constexpr int kHostLocalPort = 40410;
constexpr int kGuestLocalPort = 40411;
constexpr int kDefaultIntervalMs = 200;

ConfigKey collabKey(const char* item) {
    return ConfigKey(QString::fromLatin1(kCollabGroup),
            QString::fromLatin1(item));
}

collab::CollabConnection* connection() {
    return collab::CollabConnection::shared();
}

} // namespace

QmlCollabProxy::QmlCollabProxy(QObject* pParent)
        : QObject(pParent) {
    loadSettings();

    collab::CollabConnection* pConnection = connection();
    connect(pConnection,
            &collab::CollabConnection::stateChanged,
            this,
            &QmlCollabProxy::onConnectionStateChanged);
    connect(pConnection,
            &collab::CollabConnection::statisticsChanged,
            this,
            &QmlCollabProxy::onStatisticsChanged);
    connect(pConnection,
            &collab::CollabConnection::errorOccurred,
            this,
            &QmlCollabProxy::setLastError);
    connect(pConnection,
            &collab::CollabConnection::playApplied,
            this,
            &QmlCollabProxy::onPlayApplied);
    connect(pConnection,
            &collab::CollabConnection::localInputBlocked,
            this,
            [this](int, const QString&) { emit ownershipNoticeChanged(); });

    pConnection->setClockIntervalMs(m_intervalMs);
}

QmlCollabProxy::~QmlCollabProxy() = default;

bool QmlCollabProxy::sessionActive() const {
    return connection()->isActive();
}

bool QmlCollabProxy::connected() const {
    return connection()->isConnected();
}

bool QmlCollabProxy::clockReady() const {
    return connection()->hasClockEstimate();
}

QString QmlCollabProxy::peerAddress() const {
    return connection()->peerAddress();
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
    const int clamped = qBound(20, intervalMs, 5000);
    if (m_intervalMs == clamped) {
        return;
    }
    m_intervalMs = clamped;
    storeValue(kIntervalKey, m_intervalMs);
    connection()->setClockIntervalMs(m_intervalMs);
    emit intervalMsChanged();
}

QString QmlCollabProxy::sessionCode() const {
    return m_sessionCode;
}

void QmlCollabProxy::setSessionCode(const QString& code) {
    const QString normalized = code.trimmed().toUpper();
    if (m_sessionCode == normalized) {
        return;
    }
    m_sessionCode = normalized;
    storeValue(kSessionCodeKey, m_sessionCode);
    emit sessionCodeChanged();
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
    return static_cast<double>(connection()->rttUs()) / 1000.0;
}

double QmlCollabProxy::offsetMs() const {
    return static_cast<double>(connection()->offsetUs()) / 1000.0;
}

double QmlCollabProxy::jitterMs() const {
    return static_cast<double>(connection()->jitterUs()) / 1000.0;
}

double QmlCollabProxy::skewPpm() const {
    return connection()->skewPpm();
}

int QmlCollabProxy::sampleCount() const {
    return connection()->sampleCount();
}

QString QmlCollabProxy::lastError() const {
    return m_lastError;
}

QString QmlCollabProxy::ownershipNotice() const {
    return connection()->ownershipNotice();
}

int QmlCollabProxy::lastResponseAgeMs() const {
    return connection()->lastResponseAgeMs();
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

    // The two ports are the same pair on both peers, merely swapped. Deriving
    // them from the role removes the most common source of connection errors.
    applyRolePorts();
}

void QmlCollabProxy::applyRolePorts() {
    setLocalPort(m_hostRole ? kHostLocalPort : kGuestLocalPort);
    setPeerPort(m_hostRole ? kGuestLocalPort : kHostLocalPort);
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

bool QmlCollabProxy::lastCommandRemote() const {
    return m_lastCommandRemote;
}

bool QmlCollabProxy::startSession() {
    if (sessionActive()) {
        return true;
    }
    if (m_sessionCode.isEmpty()) {
        if (m_hostRole) {
            setSessionCode(collab::makeCollabSessionCode());
        } else {
            setLastError(tr("Enter the session code shown by the host."));
            return false;
        }
    }

    const collab::CollabSessionToken token =
            collab::collabTokenFromCode(m_sessionCode);
    const bool started = m_hostRole
            ? connection()->host(
                      static_cast<quint16>(m_localPort), token, m_diagnosticsPath)
            : connection()->join(m_peerHost,
                      static_cast<quint16>(m_peerPort),
                      static_cast<quint16>(m_localPort),
                      token,
                      m_diagnosticsPath);
    if (!started) {
        setLastError(connection()->errorString());
        return false;
    }

    setLastError(QString());
    return true;
}

void QmlCollabProxy::stopSession() {
    connection()->leave();
    setLastError(QString());
    m_scheduledCommandValid = false;
    emit lastCommandChanged();
}

void QmlCollabProxy::clearStatistics() {
    connection()->clearStatistics();
}

bool QmlCollabProxy::schedulePlayTest(int deck, int leadMs) {
    const bool scheduled = connection()->schedulePlay(deck, leadMs);
    if (!scheduled) {
        setLastError(connection()->errorString());
        return false;
    }
    m_lastCommandDeck = deck;
    m_lastCommandRemote = false;
    m_lastCommandLatenessMs = 0.0;
    m_scheduledCommandValid = false;
    setLastError(QString());
    emit lastCommandChanged();
    return true;
}

void QmlCollabProxy::cancelScheduledCommands() {
    connection()->cancelScheduledCommands();
    m_scheduledCommandValid = false;
    emit lastCommandChanged();
}

QString QmlCollabProxy::newSessionCode() {
    setSessionCode(collab::makeCollabSessionCode());
    return m_sessionCode;
}

QString QmlCollabProxy::defaultDiagnosticsPath() const {
    if (!s_pConfig) {
        return QString();
    }
    return QDir(s_pConfig->getSettingsPath())
            .filePath(QStringLiteral("collab-diagnostics.csv"));
}

void QmlCollabProxy::onConnectionStateChanged() {
    const bool active = sessionActive();
    const bool isConnected = connected();
    if (m_lastActive != active) {
        m_lastActive = active;
        emit sessionActiveChanged();
    }
    if (m_lastConnected != isConnected) {
        m_lastConnected = isConnected;
        emit connectedChanged();
    }
    emit statisticsChanged();
    emit ownershipNoticeChanged();
}

void QmlCollabProxy::onStatisticsChanged() {
    emit statisticsChanged();
}

void QmlCollabProxy::onPlayApplied(int deck, double latenessMs, bool remote) {
    m_lastCommandDeck = deck;
    m_lastCommandLatenessMs = latenessMs;
    m_lastCommandRemote = remote;
    m_scheduledCommandValid = true;
    emit lastCommandChanged();
}

void QmlCollabProxy::setLastError(const QString& error) {
    if (m_lastError == error) {
        return;
    }
    m_lastError = error;
    emit lastErrorChanged();
}

void QmlCollabProxy::loadSettings() {
    m_peerHost = loadValue(kPeerHostKey, QStringLiteral("127.0.0.1")).toString();
    m_intervalMs = loadValue(kIntervalKey, kDefaultIntervalMs).toInt();
    if (m_intervalMs <= 0) {
        m_intervalMs = kDefaultIntervalMs;
    }
    m_sessionCode = loadValue(kSessionCodeKey, QString()).toString().toUpper();
    m_diagnosticsPath = loadValue(kDiagnosticsPathKey, QString()).toString();
    if (m_diagnosticsPath.isEmpty()) {
        m_diagnosticsPath = defaultDiagnosticsPath();
    }
    m_hostRole = loadValue(kHostRoleKey, QStringLiteral("1")).toString() !=
            QStringLiteral("0");
    applyRolePorts();
}

void QmlCollabProxy::storeValue(const QString& key, const QVariant& value) {
    if (!s_pConfig) {
        return;
    }
    s_pConfig->set(collabKey(key.toLatin1().constData()), value.toString());
    s_pConfig->save();
}

QVariant QmlCollabProxy::loadValue(
        const QString& key, const QVariant& defaultValue) const {
    if (!s_pConfig) {
        return defaultValue;
    }
    return s_pConfig->getValue(
            collabKey(key.toLatin1().constData()), defaultValue.toString());
}

} // namespace qml
} // namespace mixxx
