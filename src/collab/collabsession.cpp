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

    // Close is user-initiated, so the session returns to Idle rather than to
    // the terminal Closed state that a failure produces.
    if (m_state != State::Idle) {
        setState(State::Idle);
    }
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
    return true;
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

    notePeerPacket(packet);

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
    m_remotePeerId = packet.header.senderPeerId;
    notePeerPacket(packet);

    if (m_state == State::Listening) {
        m_hostSessionTimeUs = static_cast<uint64_t>(m_pClock->nowUs());
        setState(State::Connected);
    }
    // Resend on every retry: the first WELCOME may have been lost.
    sendWelcome(sender, senderPort);
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
    m_remotePeerId = packet.header.senderPeerId;
    m_remoteAddress = sender;
    m_remotePort = senderPort;
    m_sessionEpoch = welcome.sessionEpoch;
    m_hostSessionTimeUs = welcome.hostSessionTimeUs;
    m_errorString.clear();
    notePeerPacket(packet);
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
        if (nowUs - m_handshakeStartUs >
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
            fail(tr("The peer stopped responding."));
            return;
        }
        if (nowUs - m_lastKeepaliveUs >=
                static_cast<qint64>(m_timeouts.keepaliveMs) * 1000) {
            sendKeepalive(CollabMessageType::Keepalive);
        }
    }
}

void CollabSession::setState(State state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state);
}

void CollabSession::fail(const QString& message) {
    m_errorString = message;
    if (m_pTimer != nullptr) {
        m_pTimer->stop();
    }
    if (m_pSocket != nullptr) {
        m_pSocket->close();
    }
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
}

int CollabSession::lastPeerPacketAgeMs() const {
    if (m_lastPeerPacketUs == 0) {
        return -1;
    }
    return static_cast<int>((m_pClock->nowUs() - m_lastPeerPacketUs) / 1000);
}

QString CollabSession::remoteAddress() const {
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
