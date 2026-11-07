#include "collab/collabconnection.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTimer>
#include <algorithm>

#include "collab/collabclock.h"
#include "collab/collabprotocol.h"
#include "control/controlproxy.h"
#include "moc_collabconnection.cpp"

namespace mixxx {
namespace collab {

namespace {

/// Scheduled play targets are aligned to this grid so both peers compute the
/// same target from their own clock estimate.
constexpr qint64 kPlayTestGridUs = 2 * 1000 * 1000;

constexpr int kMinLeadMs = 200;
constexpr int kSchedulerTickMs = 1;

} // namespace

CollabConnection::CollabConnection(QObject* pParent)
        : QObject(pParent),
          m_pClock(std::make_unique<SteadyCollabClock>()),
          m_pSession(new CollabSession(this)),
          m_pClockTimer(new QTimer(this)),
          m_pSchedulerTimer(new QTimer(this)) {
    m_pClockTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pClockTimer, &QTimer::timeout, this, &CollabConnection::sendClockPing);

    // One millisecond keeps the polling error below an audio buffer. The timer
    // only runs while a command is pending.
    m_pSchedulerTimer->setInterval(kSchedulerTickMs);
    m_pSchedulerTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pSchedulerTimer,
            &QTimer::timeout,
            this,
            &CollabConnection::tickScheduler);

    connect(m_pSession,
            &CollabSession::stateChanged,
            this,
            &CollabConnection::onStateChanged);
    connect(m_pSession,
            &CollabSession::packetReceived,
            this,
            &CollabConnection::onPacketReceived);
    connect(m_pSession,
            &CollabSession::errorOccurred,
            this,
            &CollabConnection::errorOccurred);
}

CollabConnection::~CollabConnection() = default;

CollabConnection* CollabConnection::shared() {
    static CollabConnection* pInstance = nullptr;
    if (pInstance == nullptr) {
        // The session outlives individual QML engines, so it belongs to the
        // application object when there is one.
        pInstance = new CollabConnection(QCoreApplication::instance());
    }
    return pInstance;
}

bool CollabConnection::host(quint16 localPort,
        const CollabSessionToken& token,
        const QString& diagnosticsPath) {
    leave();
    openDiagnostics(diagnosticsPath);
    m_clockSync.reset();
    if (!m_pSession->host(localPort, token)) {
        m_errorString = m_pSession->errorString();
        return false;
    }
    m_errorString.clear();
    return true;
}

bool CollabConnection::join(const QString& hostAddress,
        quint16 hostPort,
        quint16 localPort,
        const CollabSessionToken& token,
        const QString& diagnosticsPath) {
    const QHostAddress address(hostAddress);
    if (address.isNull()) {
        m_errorString = tr("Invalid peer address: %1").arg(hostAddress);
        emit errorOccurred(m_errorString);
        return false;
    }
    if (hostPort == 0) {
        m_errorString = tr("Peer port must not be zero");
        emit errorOccurred(m_errorString);
        return false;
    }

    leave();
    openDiagnostics(diagnosticsPath);
    m_clockSync.reset();
    if (!m_pSession->join(address, hostPort, localPort, token)) {
        m_errorString = m_pSession->errorString();
        return false;
    }
    m_errorString.clear();
    return true;
}

void CollabConnection::leave() {
    m_pClockTimer->stop();
    m_pSchedulerTimer->stop();
    m_scheduler.clear();
    m_pSession->close();
    m_diagnostics.close();
    m_lastPongUs = 0;
}

CollabSession::State CollabConnection::state() const {
    return m_pSession->state();
}

bool CollabConnection::isActive() const {
    const CollabSession::State state = m_pSession->state();
    return state == CollabSession::State::Listening ||
            state == CollabSession::State::Joining ||
            state == CollabSession::State::Connected;
}

bool CollabConnection::isConnected() const {
    return m_pSession->state() == CollabSession::State::Connected;
}

bool CollabConnection::isHost() const {
    return m_pSession->role() == CollabRole::Host;
}

QString CollabConnection::errorString() const {
    if (!m_errorString.isEmpty()) {
        return m_errorString;
    }
    return m_pSession->errorString();
}

QString CollabConnection::peerAddress() const {
    return m_pSession->remoteAddress();
}

quint16 CollabConnection::localPort() const {
    return m_pSession->localPort();
}

qint64 CollabConnection::nowUs() const {
    return m_pClock->nowUs();
}

qint64 CollabConnection::sessionNowUs() const {
    const qint64 localUs = m_pClock->nowUs();
    if (m_pSession->role() == CollabRole::Host) {
        return localUs;
    }
    return m_clockSync.toRemoteTime(localUs);
}

void CollabConnection::clearStatistics() {
    m_clockSync.reset();
    m_lastPongUs = 0;
    emit statisticsChanged();
}

int CollabConnection::lastResponseAgeMs() const {
    if (m_lastPongUs == 0) {
        return -1;
    }
    return static_cast<int>((m_pClock->nowUs() - m_lastPongUs) / 1000);
}

void CollabConnection::setClockIntervalMs(int intervalMs) {
    m_clockIntervalMs = std::max(intervalMs, 20);
    if (m_pClockTimer->isActive()) {
        m_pClockTimer->start(m_clockIntervalMs);
    }
}

bool CollabConnection::openDiagnostics(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    QString error;
    if (!m_diagnostics.open(path, &error)) {
        m_errorString = tr("Cannot open diagnostics CSV: %1").arg(error);
        emit errorOccurred(m_errorString);
        return false;
    }
    return true;
}

void CollabConnection::onStateChanged(CollabSession::State state) {
    if (state == CollabSession::State::Connected) {
        m_errorString.clear();
        m_clockSync.reset();
        m_lastPongUs = 0;
        sendClockPing();
        m_pClockTimer->start(m_clockIntervalMs);
        startControlReplication();
    } else {
        m_pClockTimer->stop();
        stopControlReplication();
    }
    emit stateChanged();
    emit statisticsChanged();
}

void CollabConnection::onPacketReceived(const CollabPacket& packet) {
    switch (packet.header.type) {
    case CollabMessageType::ClockPing:
        onClockPing(packet);
        break;
    case CollabMessageType::ClockPong:
        onClockPong(packet);
        break;
    case CollabMessageType::Command:
        onCommand(packet);
        break;
    case CollabMessageType::ControlDelta:
    case CollabMessageType::ControlKeyframe:
        m_controlReplicator.handlePacket(packet);
        break;
    default:
        break;
    }
}

void CollabConnection::sendClockPing() {
    if (!isConnected()) {
        m_pClockTimer->stop();
        return;
    }
    CollabClockPing ping;
    ping.t1Us = static_cast<uint64_t>(m_pClock->nowUs());
    if (!m_pSession->sendMessage(
                CollabMessageType::ClockPing, encodeCollabClockPing(ping))) {
        return;
    }
}

void CollabConnection::onClockPing(const CollabPacket& packet) {
    CollabClockPing ping;
    if (!decodeCollabClockPing(packet.payload, &ping)) {
        return;
    }
    CollabClockPong pong;
    pong.t1Us = ping.t1Us;
    pong.t2Us = static_cast<uint64_t>(m_pClock->nowUs());
    pong.t3Us = static_cast<uint64_t>(m_pClock->nowUs());
    m_pSession->sendMessage(
            CollabMessageType::ClockPong, encodeCollabClockPong(pong));
}

void CollabConnection::onClockPong(const CollabPacket& packet) {
    CollabClockPong pong;
    if (!decodeCollabClockPong(packet.payload, &pong)) {
        return;
    }
    const qint64 t4Us = m_pClock->nowUs();
    m_lastPongUs = t4Us;
    const bool hadEstimate = m_clockSync.hasEstimate();
    if (!m_clockSync.addSample(static_cast<qint64>(pong.t1Us),
                static_cast<qint64>(pong.t2Us),
                static_cast<qint64>(pong.t3Us),
                t4Us)) {
        return;
    }

    const CollabClockSample* pSample = m_clockSync.lastSample();
    if (pSample != nullptr) {
        m_diagnostics.logClockSample(t4Us,
                m_clockSync.toRemoteTime(t4Us),
                *pSample,
                m_clockSync.offsetUs(),
                m_clockSync.jitterUs(),
                m_clockSync.skewPpm());
    }

    emit statisticsChanged();
    if (!hadEstimate && m_clockSync.hasEstimate()) {
        // The guest can schedule commands from now on; the host does not care.
        emit stateChanged();
    }
}

void CollabConnection::startControlReplication() {
    if (!m_controlReplicationEnabled) {
        return;
    }
    // Both peers build the map from the same session-space key list, so the
    // identifiers match without exchanging the map yet.
    const CollabControlMap map =
            CollabControlMap::fromKeys(collabDefaultControlKeys());
    const CollabDeckMap deckMap(m_pSession->role());
    if (!m_controlReplicator.start(
                map,
                deckMap,
                [this](CollabMessageType type, const QByteArray& payload) {
                    return m_pSession->sendMessage(type, payload);
                })) {
        m_errorString = m_controlReplicator.errorString();
        emit errorOccurred(m_errorString);
        return;
    }
    // The peer applies a full snapshot before it receives any delta.
    m_controlReplicator.sendKeyframe();
}

void CollabConnection::stopControlReplication() {
    m_controlReplicator.stop();
}

void CollabConnection::onCommand(const CollabPacket& packet) {
    CollabCommand command;
    if (!decodeCollabCommand(packet.payload, &command)) {
        return;
    }
    if (command.action != CollabCommandAction::Play) {
        return;
    }
    if (m_scheduler.isKnown(command.commandId)) {
        // Duplicate datagram; both peers already have this command.
        return;
    }
    if (!isHost() && !m_clockSync.hasEstimate()) {
        m_errorString = tr("Ignored a remote command before the clock was synced");
        emit errorOccurred(m_errorString);
        return;
    }
    scheduleLocalPlay(command.deckId,
            static_cast<qint64>(command.targetSessionTimeUs),
            command.commandId,
            true);
}

bool CollabConnection::schedulePlay(int deck, int leadMs) {
    if (deck < 1 || deck > kCollabSessionDeckCount) {
        m_errorString = tr("Deck must be between 1 and 4");
        emit errorOccurred(m_errorString);
        return false;
    }
    if (!isConnected()) {
        m_errorString = tr("Connect to the peer before scheduling a play");
        emit errorOccurred(m_errorString);
        return false;
    }
    if (!isHost() && !m_clockSync.hasEstimate()) {
        m_errorString = tr("Wait for the clock estimate before scheduling a play");
        emit errorOccurred(m_errorString);
        return false;
    }

    const qint64 nowUs = sessionNowUs();
    const qint64 requestedUs =
            nowUs + static_cast<qint64>(std::max(leadMs, kMinLeadMs)) * 1000;
    // Next grid boundary strictly after the requested time.
    const qint64 targetUs = ((requestedUs / kPlayTestGridUs) + 1) * kPlayTestGridUs;

    const quint64 commandId = makeCommandId();
    if (!scheduleLocalPlay(deck, targetUs, commandId, false)) {
        return false;
    }

    CollabCommand command;
    command.commandId = commandId;
    command.action = CollabCommandAction::Play;
    command.deckId = static_cast<uint16_t>(deck);
    command.targetSessionTimeUs = static_cast<uint64_t>(targetUs);
    if (!m_pSession->sendMessage(
                CollabMessageType::Command, encodeCollabCommand(command))) {
        m_scheduler.cancel(commandId);
        if (m_scheduler.pendingCount() == 0) {
            m_pSchedulerTimer->stop();
        }
        m_errorString = tr("Could not send the play command: %1")
                                .arg(m_pSession->errorString());
        emit errorOccurred(m_errorString);
        return false;
    }

    m_errorString.clear();
    return true;
}

bool CollabConnection::scheduleLocalPlay(
        int deck, qint64 targetSessionUs, quint64 commandId, bool remote) {
    if (deck < 1 || deck > kCollabSessionDeckCount) {
        return false;
    }

    auto pPlayProxy = std::make_shared<ControlProxy>(
            ConfigKey(QStringLiteral("[Channel%1]").arg(deck),
                    QStringLiteral("play")),
            this);
    if (!pPlayProxy->valid()) {
        m_errorString = tr("Deck %1 is not available").arg(deck);
        emit errorOccurred(m_errorString);
        return false;
    }
    // Start from a stopped deck so a play command is always a transition.
    pPlayProxy->set(0.0);

    const bool scheduled = m_scheduler.schedule(
            commandId,
            targetSessionUs,
            [this, pPlayProxy, commandId, targetSessionUs, remote, deck](
                    CollabCommandScheduler::CommandId,
                    qint64 latenessUs) {
                pPlayProxy->set(1.0);
                if (m_diagnostics.isOpen()) {
                    m_diagnostics.logScheduledCommand(commandId,
                            targetSessionUs,
                            targetSessionUs + latenessUs,
                            latenessUs,
                            false);
                }
                emit playApplied(
                        deck, static_cast<double>(latenessUs) / 1000.0, remote);
            });
    if (!scheduled) {
        m_errorString = tr("Could not schedule deck %1; is it already pending?")
                                .arg(deck);
        emit errorOccurred(m_errorString);
        return false;
    }

    m_pSchedulerTimer->start();
    return true;
}

void CollabConnection::tickScheduler() {
    m_scheduler.runDue(sessionNowUs());
    if (m_scheduler.pendingCount() == 0) {
        m_pSchedulerTimer->stop();
    }
}

void CollabConnection::cancelScheduledCommands() {
    m_scheduler.clear();
    m_pSchedulerTimer->stop();
}

quint64 CollabConnection::makeCommandId() {
    // The low bit encodes the role, so the two peers never generate the same
    // identifier even though both count from one.
    return (m_nextCommandId++ << 1) | (isHost() ? 0ULL : 1ULL);
}

} // namespace collab
} // namespace mixxx
