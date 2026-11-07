#include "collab/collabsession.h"

#include <QTimer>
#include <QUdpSocket>

#include "collab/collabclock.h"
#include "moc_collabsession.cpp"

namespace mixxx {
namespace collab {

namespace {

constexpr uint16_t kRejectWrongToken = 1;
constexpr uint16_t kRejectWrongSession = 2;
constexpr uint16_t kRejectBusy = 3;

constexpr int kTimerIntervalMs = 50;

} // namespace

CollabSession::CollabSession(QObject* pParent)
        : QObject(pParent),
          m_pClock(std::make_unique<SteadyCollabClock>()),
          m_pSocket(new QUdpSocket(this)),
          m_pTimer(new QTimer(this)) {
    m_pTimer->setTimerType(Qt::PreciseTimer);
    m_pTimer->setInterval(kTimerIntervalMs);
    connect(m_pTimer, &QTimer::timeout, this, &CollabSession::onTimer);
    connect(m_pSocket, &QUdpSocket::readyRead, this, [this]() {
        // A handler can close the socket while datagrams are still queued.
        while (m_pSocket->state() == QAbstractSocket::BoundState &&
                m_pSocket->hasPendingDatagrams()) {
            QByteArray data;
            data.resize(static_cast<qsizetype>(m_pSocket->pendingDatagramSize()));
            QHostAddress sender;
            quint16 senderPort = 0;
            const qint64 read = m_pSocket->readDatagram(
                    data.data(), data.size(), &sender, &senderPort);
            if (read < 0) {
                continue;
            }
            data.resize(static_cast<qsizetype>(read));
            onDatagram(data, sender, senderPort);
        }
    });
}

CollabSession::~CollabSession() = default;

bool CollabSession::host(quint16 localPort, const CollabSessionToken& token) {
    close();
    if (!bindSocket(localPort)) {
        return false;
    }

    m_role = CollabRole::Host;
    m_token = token;
    m_sessionId = makeCollabSessionId();
    m_localPeerId = makeCollabPeerId();
    m_remotePeerId = CollabPeerId{};
    m_sessionEpoch = 1;
    m_hostSessionTimeUs = 0;
    m_lastPeerPacketUs = 0;
    m_lastReceivedSeq = 0;
    m_nextSeq = 1;
    m_errorString.clear();

    setState(State::Listening);
    m_pTimer->start();
    return true;
}

bool CollabSession::join(const QHostAddress& hostAddress,
        quint16 hostPort,
        quint16 localPort,
        const CollabSessionToken& token) {
    close();
    if (!bindSocket(localPort)) {
        return false;
    }

    m_role = CollabRole::Guest;
    m_token = token;
    m_hostAddress = hostAddress;
    m_hostPort = hostPort;
    m_sessionId = CollabSessionId{};
    m_localPeerId = makeCollabPeerId();
    m_remotePeerId = CollabPeerId{};
    m_sessionEpoch = 0;
    m_hostSessionTimeUs = 0;
    m_remoteAddress = QHostAddress();
    m_remotePort = 0;
    m_lastPeerPacketUs = 0;
    m_lastReceivedSeq = 0;
    m_nextSeq = 1;
    m_lastHelloUs = 0;
    m_handshakeStartUs = m_pClock->nowUs();
    m_errorString.clear();
    clearReliable();

    setState(State::Joining);
    m_pTimer->start();
    sendHello();
    return true;
}

void CollabSession::close() {
    if (m_state == State::Connected) {
        sendBye();
    }
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    if (m_pSocket != nullptr) {
        m_pSocket->close();
    }
    m_remoteAddress = QHostAddress();
    m_remotePort = 0;
    m_lastPeerPacketUs = 0;
    m_lastHelloUs = 0;
    m_lastKeepaliveUs = 0;
    m_handshakeStartUs = 0;
    m_sequenceTracker.reset();
    clearReliable();
    setRecovering(false);

    // Close is user-initiated, so the session returns to Idle rather than to
    // the terminal Closed state that a failure produces.
    if (m_state != State::Idle) {
        setState(State::Idle);
    }
}

bool CollabSession::reconnect() {
    if (m_pSocket == nullptr ||
            m_pSocket->state() != QAbstractSocket::BoundState) {
        return false;
    }

    setRecovering(true);
    m_lastPeerPacketUs = 0;
    m_lastKeepaliveUs = 0;
    m_errorString.clear();

    if (m_role == CollabRole::Host) {
        // Keep the session id, the remote endpoint and the peer id. A guest
        // that has not noticed the drop keeps sending keepalives with the old
        // session id; accepting one revives the session without a new
        // handshake. A guest that did notice sends a fresh HELLO, which is
        // handled by the normal Listening path.
        setState(State::Listening);
    } else {
        m_sessionId = CollabSessionId{};
        m_remotePeerId = CollabPeerId{};
        m_remoteAddress = QHostAddress();
        m_remotePort = 0;
        m_hostSessionTimeUs = 0;
        m_lastHelloUs = 0;
        m_handshakeStartUs = m_pClock->nowUs();
        setState(State::Joining);
        sendHello();
    }

    m_pTimer->start();
    return true;
}

bool CollabSession::sendMessage(CollabMessageType type,
        const QByteArray& payload,
        uint16_t flags) {
    if (m_state != State::Connected || m_remotePort == 0) {
        return false;
    }
    return sendTo(m_remoteAddress, m_remotePort, type, payload, flags, m_sessionId);
}

bool CollabSession::bindSocket(quint16 localPort) {
    if (m_pSocket->state() != QAbstractSocket::UnconnectedState) {
        m_pSocket->close();
    }
    if (!m_pSocket->bind(QHostAddress::Any, localPort)) {
        m_errorString = tr("Cannot bind UDP port %1: %2")
                                .arg(localPort)
                                .arg(m_pSocket->errorString());
        emit errorOccurred(m_errorString);
        return false;
    }
    return true;
}

bool CollabSession::sendTo(const QHostAddress& address,
        quint16 port,
        CollabMessageType type,
        const QByteArray& payload,
        uint16_t flags,
        const CollabSessionId& sessionId) {
    CollabHeader header;
    header.type = type;
    header.flags = flags;
    header.sessionId = sessionId;
    header.senderPeerId = m_localPeerId;
    header.seq = m_nextSeq++;
    header.ack = m_lastReceivedSeq;
    header.ackBits = 0;
    header.sendTimeUs = static_cast<uint64_t>(m_pClock->nowUs());

    const QByteArray data = encodeCollabPacket(header, payload);
    if (m_pSocket->writeDatagram(data, address, port) != data.size()) {
        return false;
    }
    if ((flags & CollabFlagReliable) != 0) {
        bufferReliable(data, address, port, header.seq);
    }
    return true;
}

void CollabSession::bufferReliable(const QByteArray& datagram,
        const QHostAddress& address,
        quint16 port,
        uint32_t seq) {
    while (m_pendingReliable.size() >= kMaxPendingReliable) {
        // Memory bound: a peer that never acknowledges must not grow the
        // queue without limit. The oldest message is dropped; the session
        // timeout will end the session long before this matters.
        m_pendingReliable.removeFirst();
    }
    PendingReliable pending;
    pending.datagram = datagram;
    pending.address = address;
    pending.port = port;
    pending.seq = seq;
    pending.firstSendUs = m_pClock->nowUs();
    pending.lastSendUs = pending.firstSendUs;
    pending.attempts = 1;
    m_pendingReliable.append(pending);
}

void CollabSession::pumpReliable(qint64 nowUs) {
    if (m_pendingReliable.isEmpty()) {
        return;
    }
    const qint64 retryUs = static_cast<qint64>(m_timeouts.reliableRetryMs) * 1000;
    for (int i = m_pendingReliable.size() - 1; i >= 0; --i) {
        PendingReliable& pending = m_pendingReliable[i];
        if (nowUs - pending.lastSendUs < retryUs) {
            continue;
        }
        if (pending.attempts >= m_timeouts.reliableMaxAttempts ||
                pending.seq <= m_lastReceivedSeq) {
            // Either the retries are exhausted, or the peer's own sequence
            // numbers show it received later packets, so the message had its
            // chance (its receivers deduplicate either way).
            m_pendingReliable.removeAt(i);
            continue;
        }
        if (m_pSocket->writeDatagram(
                    pending.datagram, pending.address, pending.port) ==
                pending.datagram.size()) {
            ++pending.attempts;
            pending.lastSendUs = nowUs;
        } else {
            m_pendingReliable.removeAt(i);
        }
    }
}

void CollabSession::clearReliable() {
    m_pendingReliable.clear();
}

void CollabSession::sendHello() {
    CollabHello hello;
    hello.role = m_role;
    hello.token = m_token;
    hello.featureFlags = 0;

    // The guest does not know the session id yet; the host answers with it.
    sendTo(m_hostAddress,
            m_hostPort,
            CollabMessageType::Hello,
            encodeCollabHello(hello),
            CollabFlagReliable | CollabFlagRequestAck,
            CollabSessionId{});
    m_lastHelloUs = m_pClock->nowUs();
}

void CollabSession::sendWelcome(const QHostAddress& address, quint16 port) {
    CollabWelcome welcome;
    welcome.sessionEpoch = m_sessionEpoch;
    welcome.role = CollabRole::Guest;
    welcome.hostSessionTimeUs = m_hostSessionTimeUs;

    sendTo(address,
            port,
            CollabMessageType::Welcome,
            encodeCollabWelcome(welcome),
            CollabFlagReliable | CollabFlagRequestAck | CollabFlagResponse,
            m_sessionId);
}

void CollabSession::sendReject(const QHostAddress& address,
        quint16 port,
        uint16_t reasonCode,
        const QString& message,
        const CollabSessionId& sessionId) {
    CollabBye reject;
    reject.reasonCode = reasonCode;
    reject.message = message;

    sendTo(address,
            port,
            CollabMessageType::Reject,
            encodeCollabBye(reject),
            CollabFlagReliable | CollabFlagRequestAck | CollabFlagResponse,
            sessionId);
}

void CollabSession::sendKeepalive(CollabMessageType type) {
    if (m_state != State::Connected || m_remotePort == 0) {
        return;
    }
    sendTo(m_remoteAddress, m_remotePort, type, QByteArray(), CollabFlagNone, m_sessionId);
    m_lastKeepaliveUs = m_pClock->nowUs();
}

void CollabSession::sendBye() {
    if (m_remotePort == 0) {
        return;
    }
    CollabBye bye;
    bye.message = tr("leaving");
    sendTo(m_remoteAddress,
            m_remotePort,
            CollabMessageType::Bye,
            encodeCollabBye(bye),
            CollabFlagReliable,
            m_sessionId);
}

void CollabSession::onDatagram(const QByteArray& data,
        const QHostAddress& sender,
        quint16 senderPort) {
    CollabPacket packet;
    if (!decodeCollabPacket(data, &packet)) {
        return;
    }

    switch (packet.header.type) {
    case CollabMessageType::Hello:
        if (m_role == CollabRole::Host) {
            handleHello(packet, sender, senderPort);
        }
        return;
    case CollabMessageType::Welcome:
        if (m_role == CollabRole::Guest) {
            handleWelcome(packet, sender, senderPort);
        }
        return;
    case CollabMessageType::Reject:
        if (m_role == CollabRole::Guest) {
            handleReject(packet, sender, senderPort);
        }
        return;
    default:
        break;
    }

    if (m_state == State::Listening && m_role == CollabRole::Host &&
            m_recovering &&
            (packet.header.type == CollabMessageType::Keepalive ||
                    packet.header.type == CollabMessageType::KeepaliveAck) &&
            packet.header.sessionId == m_sessionId &&
            packet.header.senderPeerId == m_remotePeerId &&
            endpointMatches(sender, senderPort)) {
        // The guest never noticed the drop and is still sending keepalives.
        // Revive the session without a handshake; the caller restarts its
        // replication on the Connected transition.
        notePeerPacket(packet);
        setRecovering(false);
        setState(State::Connected);
        sendKeepalive(CollabMessageType::KeepaliveAck);
        return;
    }

    if (m_state != State::Connected) {
        return;
    }
    if (packet.header.sessionId != m_sessionId) {
        return;
    }
    if (packet.header.senderPeerId != m_remotePeerId) {
        return;
    }
    if (!endpointMatches(sender, senderPort)) {
        return;
    }

    const bool duplicate = m_sequenceTracker.seen(packet.header.seq);
    notePeerPacket(packet);

    if (duplicate && packet.header.seq != 0) {
        // A retransmitted reliable datagram. The ack field of our own next
        // packet confirms it again, but delivering it twice would replay
        // load requests and other state changes ("its receivers deduplicate
        // either way" finally became true).
        if (packet.header.type == CollabMessageType::Keepalive) {
            sendKeepalive(CollabMessageType::KeepaliveAck);
        }
        return;
    }

    if (packet.header.type == CollabMessageType::Keepalive) {
        sendKeepalive(CollabMessageType::KeepaliveAck);
        return;
    }
    if (packet.header.type == CollabMessageType::KeepaliveAck) {
        return;
    }
    if (packet.header.type == CollabMessageType::Bye) {
        fail(tr("The peer left the session."));
        return;
    }

    emit packetReceived(packet);
}

void CollabSession::handleHello(const CollabPacket& packet,
        const QHostAddress& sender,
        quint16 senderPort) {
    CollabHello hello;
    if (!decodeCollabHello(packet.payload, &hello)) {
        return;
    }
    if (!collabTokensEqual(m_token, hello.token)) {
        // The sender is not authenticated, so do not disclose the session id.
        sendReject(sender,
                senderPort,
                kRejectWrongToken,
                tr("The session token does not match."),
                CollabSessionId{});
        return;
    }
    // Once the guest knows the session id it must use it.
    const CollabSessionId noSession{};
    if (packet.header.sessionId != noSession &&
            packet.header.sessionId != m_sessionId) {
        sendReject(sender,
                senderPort,
                kRejectWrongSession,
                tr("This peer belongs to another session."),
                packet.header.sessionId);
        return;
    }
    if (m_state == State::Connected && !endpointMatches(sender, senderPort)) {
        sendReject(sender,
                senderPort,
                kRejectBusy,
                tr("The host is already in a session."),
                m_sessionId);
        return;
    }

    m_remoteAddress = sender;
    m_remotePort = senderPort;
    if (m_remotePeerId != packet.header.senderPeerId) {
        // A peer that restarted the process starts its sequence from one, so
        // the old window no longer applies.
        m_sequenceTracker.reset();
    }
    m_remotePeerId = packet.header.senderPeerId;
    notePeerPacket(packet);

    const bool wasConnected = m_state == State::Connected;
    if (m_state == State::Listening) {
        m_hostSessionTimeUs = static_cast<uint64_t>(m_pClock->nowUs());
        // Handshake retries consume sequence numbers that the peer never
        // accepts, so start the loss window at the first session packet.
        m_sequenceTracker.reset();
        setRecovering(false);
        setState(State::Connected);
    }
    // Resend on every retry: the first WELCOME may have been lost.
    sendWelcome(sender, senderPort);
    if (wasConnected) {
        // The peer restarted its handshake while this side stayed connected.
        // It has a fresh replicator, so it needs a snapshot immediately.
        emit peerRejoined();
    }
}

void CollabSession::handleWelcome(const CollabPacket& packet,
        const QHostAddress& sender,
        quint16 senderPort) {
    if (m_state != State::Joining) {
        return;
    }
    // Only the endpoint we joined may complete the handshake.
    if (senderPort != m_hostPort || !sender.isEqual(m_hostAddress)) {
        return;
    }
    const CollabSessionId noSession{};
    if (packet.header.sessionId == noSession) {
        return;
    }
    if (packet.header.senderPeerId == m_localPeerId) {
        return;
    }
    CollabWelcome welcome;
    if (!decodeCollabWelcome(packet.payload, &welcome)) {
        return;
    }

    m_sessionId = packet.header.sessionId;
    if (m_remotePeerId != packet.header.senderPeerId) {
        m_sequenceTracker.reset();
    }
    m_remotePeerId = packet.header.senderPeerId;
    m_remoteAddress = sender;
    m_remotePort = senderPort;
    m_sessionEpoch = welcome.sessionEpoch;
    m_hostSessionTimeUs = welcome.hostSessionTimeUs;
    m_errorString.clear();
    // Handshake retries consume sequence numbers that this side never
    // accepts, so start the loss window at the first session packet.
    m_sequenceTracker.reset();
    notePeerPacket(packet);
    setRecovering(false);
    setState(State::Connected);

    // Tell the host immediately that the handshake completed.
    sendKeepalive(CollabMessageType::Keepalive);
}

void CollabSession::handleReject(const CollabPacket& packet,
        const QHostAddress& sender,
        quint16 senderPort) {
    if (senderPort != m_hostPort || !sender.isEqual(m_hostAddress)) {
        return;
    }
    CollabBye reject;
    QString message = tr("The host rejected the join.");
    if (decodeCollabBye(packet.payload, &reject) && !reject.message.isEmpty()) {
        message = reject.message;
    }
    fail(message);
}

void CollabSession::onTimer() {
    const qint64 nowUs = m_pClock->nowUs();

    if (m_state == State::Joining) {
        // While recovering, keep retrying instead of giving up: the peer may
        // come back at any time.
        if (!m_recovering &&
                nowUs - m_handshakeStartUs >
                        static_cast<qint64>(m_timeouts.handshakeMs) * 1000) {
            fail(tr("The host did not answer. Check the address and the firewall."));
            return;
        }
        if (nowUs - m_lastHelloUs >=
                static_cast<qint64>(m_timeouts.handshakeRetryMs) * 1000) {
            sendHello();
        }
        return;
    }

    if (m_state == State::Connected) {
        if (m_lastPeerPacketUs != 0 &&
                nowUs - m_lastPeerPacketUs >
                        static_cast<qint64>(m_timeouts.peerTimeoutMs) * 1000) {
            if (m_autoReconnect) {
                reconnect();
            } else {
                fail(tr("The peer stopped responding."));
            }
            return;
        }
        if (nowUs - m_lastKeepaliveUs >=
                static_cast<qint64>(m_timeouts.keepaliveMs) * 1000) {
            sendKeepalive(CollabMessageType::Keepalive);
        }
        pumpReliable(nowUs);
    }
}

void CollabSession::setState(State state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

void CollabSession::setRecovering(bool recovering) {
    if (m_recovering == recovering) {
        return;
    }
    m_recovering = recovering;
    emit recoveringChanged(m_recovering);
}

void CollabSession::fail(const QString& message) {
    m_errorString = message;
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    if (m_pSocket != nullptr) {
        m_pSocket->close();
    }
    clearReliable();
    setRecovering(false);
    setState(State::Closed);
    emit errorOccurred(message);
}

bool CollabSession::endpointMatches(
        const QHostAddress& address, quint16 port) const {
    return m_remotePort != 0 && port == m_remotePort &&
            address.isEqual(m_remoteAddress);
}

void CollabSession::notePeerPacket(const CollabPacket& packet) {
    m_lastPeerPacketUs = m_pClock->nowUs();
    m_lastReceivedSeq = packet.header.seq;
    m_sequenceTracker.note(packet.header.seq);
}

int CollabSession::lastPeerPacketAgeMs() const {
    if (m_lastPeerPacketUs == 0) {
        return -1;
    }
    return static_cast<int>((m_pClock->nowUs() - m_lastPeerPacketUs) / 1000);
}

QString CollabSession::remoteAddress() const {
    // A socket bound to QHostAddress::Any reports IPv4 peers as IPv4-mapped
    // IPv6 addresses; show them in their readable form.
    bool converted = false;
    const quint32 ipv4 = m_remoteAddress.toIPv4Address(&converted);
    if (converted) {
        return QHostAddress(ipv4).toString();
    }
    return m_remoteAddress.toString();
}

quint16 CollabSession::localPort() const {
    return m_pSocket->localPort();
}

void CollabSession::setTimeouts(const CollabSessionTimeouts& timeouts) {
    m_timeouts = timeouts;
}

} // namespace collab
} // namespace mixxx
