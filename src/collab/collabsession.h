#pragma once

#include <QHostAddress>
#include <QObject>
#include <QString>
#include <cstdint>
#include <memory>

#include "collab/collabprotocol.h"
#include "collab/collabtypes.h"

class QTimer;
class QUdpSocket;

namespace mixxx {
namespace collab {

class SteadyCollabClock;

/// Tunable timing for the handshake and the liveness check.
struct CollabSessionTimeouts {
    /// How long a joining guest keeps retrying before it gives up.
    int handshakeMs = 10000;
    /// Delay between two join attempts.
    int handshakeRetryMs = 400;
    /// Delay between keepalives once connected.
    int keepaliveMs = 500;
    /// Silence after which the peer is considered gone.
    int peerTimeoutMs = 2500;
};

/// One side of a two-peer collaboration session.
///
/// The class owns the UDP socket and the session state machine. It is
/// deliberately transport-only: clock estimation, control replication and
/// command scheduling live in the classes layered on top of it.
///
/// The host binds its port and waits. The guest binds its own port and sends
/// HELLO to the host, which discovers the guest's endpoint from the datagram.
/// The token is compared in constant time and a mismatch is answered with
/// REJECT.
class CollabSession : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,
        /// Host is bound and waiting for a guest.
        Listening,
        /// Guest is sending HELLO and waiting for WELCOME.
        Joining,
        /// Both peers have agreed on a session and exchange keepalives.
        Connected,
        /// The session ended because of a timeout or a rejection.
        Closed,
    };
    Q_ENUM(State)

    explicit CollabSession(QObject* pParent = nullptr);
    ~CollabSession() override;

    /// Binds `localPort` and waits for one guest. Returns false when the port
    /// cannot be bound.
    bool host(quint16 localPort, const CollabSessionToken& token);

    /// Binds `localPort` and joins the host at `hostAddress:hostPort`.
    bool join(const QHostAddress& hostAddress,
            quint16 hostPort,
            quint16 localPort,
            const CollabSessionToken& token);

    /// Leaves the session and returns to Idle. Sends BYE when connected.
    void close();

    /// Sends one message to the connected peer. Returns false when the session
    /// is not connected or the datagram could not be written.
    bool sendMessage(CollabMessageType type,
            const QByteArray& payload,
            uint16_t flags = CollabFlagNone);

    State state() const {
        return m_state;
    }
    CollabRole role() const {
        return m_role;
    }
    CollabSessionId sessionId() const {
        return m_sessionId;
    }
    CollabPeerId localPeerId() const {
        return m_localPeerId;
    }
    CollabPeerId remotePeerId() const {
        return m_remotePeerId;
    }
    uint32_t sessionEpoch() const {
        return m_sessionEpoch;
    }
    QString remoteAddress() const;
    quint16 remotePort() const {
        return m_remotePort;
    }
    /// Host session time announced in WELCOME. Zero on the host and before the
    /// handshake completed.
    uint64_t hostSessionTimeUs() const {
        return m_hostSessionTimeUs;
    }
    /// Milliseconds since the last valid packet from the peer, or -1.
    int lastPeerPacketAgeMs() const;
    QString errorString() const {
        return m_errorString;
    }
    quint16 localPort() const;

    /// Overrides the defaults, mainly for tests.
    void setTimeouts(const CollabSessionTimeouts& timeouts);

  signals:
    void stateChanged(mixxx::collab::CollabSession::State state);
    /// Terminal problem such as a rejected token, a handshake timeout, or a
    /// peer that stopped answering.
    void errorOccurred(const QString& message);
    /// A valid message from the connected peer. Keepalives, HELLO, WELCOME and
    /// REJECT are consumed internally.
    void packetReceived(const mixxx::collab::CollabPacket& packet);

  private:
    void onDatagram(const QByteArray& data,
            const QHostAddress& sender,
            quint16 senderPort);
    void onTimer();
    void handleHello(const CollabPacket& packet,
            const QHostAddress& sender,
            quint16 senderPort);
    void handleWelcome(const CollabPacket& packet,
            const QHostAddress& sender,
            quint16 senderPort);
    void handleReject(const CollabPacket& packet,
            const QHostAddress& sender,
            quint16 senderPort);

    bool bindSocket(quint16 localPort);
    bool sendTo(const QHostAddress& address,
            quint16 port,
            CollabMessageType type,
            const QByteArray& payload,
            uint16_t flags,
            const CollabSessionId& sessionId);
    void sendHello();
    void sendWelcome(const QHostAddress& address, quint16 port);
    void sendReject(const QHostAddress& address,
            quint16 port,
            uint16_t reasonCode,
            const QString& message,
            const CollabSessionId& sessionId);
    void sendKeepalive(CollabMessageType type);
    void sendBye();

    void setState(State state);
    void fail(const QString& message);
    bool endpointMatches(const QHostAddress& address, quint16 port) const;
    void notePeerPacket(const CollabPacket& packet);

    std::unique_ptr<SteadyCollabClock> m_pClock;
    QUdpSocket* m_pSocket;
    QTimer* m_pTimer;
    CollabSessionTimeouts m_timeouts;
    State m_state = State::Idle;
    CollabRole m_role = CollabRole::Host;
    CollabSessionId m_sessionId{};
    CollabPeerId m_localPeerId{};
    CollabPeerId m_remotePeerId{};
    CollabSessionToken m_token{};
    QHostAddress m_remoteAddress;
    quint16 m_remotePort = 0;
    QHostAddress m_hostAddress;
    quint16 m_hostPort = 0;
    uint32_t m_sessionEpoch = 0;
    uint64_t m_hostSessionTimeUs = 0;
    uint32_t m_nextSeq = 1;
    uint32_t m_lastReceivedSeq = 0;
    qint64 m_lastPeerPacketUs = 0;
    qint64 m_lastHelloUs = 0;
    qint64 m_lastKeepaliveUs = 0;
    qint64 m_handshakeStartUs = 0;
    QString m_errorString;
};

} // namespace collab
} // namespace mixxx
