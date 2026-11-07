#include "collab/collabsession.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QThread>
#include <QUdpSocket>
#include <functional>

#include "collab/collabprotocol.h"

namespace mixxx {
namespace collab {

namespace {

bool waitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return predicate();
}

CollabSessionTimeouts fastTimeouts() {
    CollabSessionTimeouts timeouts;
    timeouts.handshakeMs = 1500;
    timeouts.handshakeRetryMs = 50;
    timeouts.keepaliveMs = 150;
    timeouts.peerTimeoutMs = 600;
    return timeouts;
}

/// A UDP socket that never answers, used to occupy a port.
struct SilentPort {
    QUdpSocket socket;
    quint16 port = 0;

    bool open() {
        if (!socket.bind(QHostAddress::Any, 0)) {
            return false;
        }
        port = socket.localPort();
        return true;
    }
};

} // namespace

TEST(CollabSessionTest, HostAndGuestConnect) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());

    ASSERT_TRUE(host.host(0, token));
    ASSERT_NE(host.localPort(), 0);
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));

    EXPECT_EQ(host.state(), CollabSession::State::Listening);
    EXPECT_EQ(guest.state(), CollabSession::State::Joining);

    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    EXPECT_EQ(host.role(), CollabRole::Host);
    EXPECT_EQ(guest.role(), CollabRole::Guest);
    EXPECT_EQ(host.sessionId(), guest.sessionId());
    EXPECT_NE(host.sessionId(), CollabSessionId{});
    EXPECT_EQ(host.remotePeerId(), guest.localPeerId());
    EXPECT_EQ(guest.remotePeerId(), host.localPeerId());
    EXPECT_NE(guest.hostSessionTimeUs(), 0u);
    EXPECT_EQ(host.sessionEpoch(), 1u);
    EXPECT_EQ(guest.sessionEpoch(), 1u);
}

TEST(CollabSessionTest, CountsReceivedPacketsWithoutLossOnLoopback) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    ASSERT_TRUE(waitFor(
            [&]() {
                return host.receivedPacketCount() > 0 &&
                        guest.receivedPacketCount() > 0;
            },
            3000));
    EXPECT_EQ(host.lostPacketCount(), 0u);
    EXPECT_EQ(guest.lostPacketCount(), 0u);
    EXPECT_DOUBLE_EQ(host.packetLossPercent(), 0.0);
    EXPECT_DOUBLE_EQ(guest.packetLossPercent(), 0.0);
}

TEST(CollabSessionTest, AutoReconnectRestoresTheSession) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setAutoReconnect(true);
    guest.setAutoReconnect(true);
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    ASSERT_TRUE(host.reconnect());
    ASSERT_TRUE(guest.reconnect());
    EXPECT_TRUE(host.isRecovering());
    EXPECT_TRUE(guest.isRecovering());

    EXPECT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));
    EXPECT_FALSE(host.isRecovering());
    EXPECT_FALSE(guest.isRecovering());
    EXPECT_EQ(host.sessionId(), guest.sessionId());
}

TEST(CollabSessionTest, HostRecoversFromAnUnawareGuest) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setAutoReconnect(true);
    guest.setAutoReconnect(true);
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    // The host loses the peer but the guest never notices. The guest's next
    // keepalive carries the same session and endpoint, so the host can revive
    // the session without a handshake.
    ASSERT_TRUE(host.reconnect());
    EXPECT_EQ(host.state(), CollabSession::State::Listening);
    EXPECT_TRUE(host.isRecovering());
    EXPECT_TRUE(waitFor(
            [&]() { return host.state() == CollabSession::State::Connected; },
            2000));
    EXPECT_FALSE(host.isRecovering());
    EXPECT_EQ(guest.state(), CollabSession::State::Connected);
}

TEST(CollabSessionTest, WrongTokenIsRejected) {
    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());

    ASSERT_TRUE(host.host(0, makeCollabSessionToken()));
    ASSERT_TRUE(guest.join(
            QHostAddress::LocalHost, host.localPort(), 0, makeCollabSessionToken()));

    ASSERT_TRUE(waitFor(
            [&]() { return guest.state() == CollabSession::State::Closed; }, 3000));
    EXPECT_EQ(host.state(), CollabSession::State::Listening);
    EXPECT_TRUE(guest.errorString().contains(QStringLiteral("token"),
            Qt::CaseInsensitive));
}

TEST(CollabSessionTest, ByeClosesTheOtherPeer) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    guest.close();
    EXPECT_TRUE(waitFor(
            [&]() { return host.state() == CollabSession::State::Closed; }, 3000));
    EXPECT_TRUE(host.errorString().contains(QStringLiteral("left")));
}

TEST(CollabSessionTest, SilentPeerTimesOut) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    host.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));

    // Send one valid HELLO from a raw socket, then never answer again.
    QUdpSocket raw;
    ASSERT_TRUE(raw.bind(QHostAddress::Any, 0));

    CollabHello hello;
    hello.role = CollabRole::Guest;
    hello.token = token;
    CollabHeader header;
    header.type = CollabMessageType::Hello;
    header.senderPeerId = makeCollabPeerId();
    header.seq = 1;
    const QByteArray datagram =
            encodeCollabPacket(header, encodeCollabHello(hello));
    ASSERT_EQ(raw.writeDatagram(datagram, QHostAddress::LocalHost, host.localPort()),
            datagram.size());

    ASSERT_TRUE(waitFor(
            [&]() { return host.state() == CollabSession::State::Connected; }, 3000));
    EXPECT_TRUE(waitFor(
            [&]() { return host.state() == CollabSession::State::Closed; }, 3000));
    EXPECT_TRUE(host.errorString().contains(QStringLiteral("responding")));
}

TEST(CollabSessionTest, HandshakeTimeoutIsReported) {
    SilentPort silent;
    ASSERT_TRUE(silent.open());

    CollabSession guest;
    CollabSessionTimeouts timeouts = fastTimeouts();
    timeouts.handshakeMs = 400;
    guest.setTimeouts(timeouts);

    ASSERT_TRUE(guest.join(
            QHostAddress::LocalHost, silent.port, 0, makeCollabSessionToken()));
    EXPECT_TRUE(waitFor(
            [&]() { return guest.state() == CollabSession::State::Closed; }, 3000));
    EXPECT_EQ(guest.state(), CollabSession::State::Closed);
    EXPECT_TRUE(guest.errorString().contains(QStringLiteral("did not answer")));
}

TEST(CollabSessionTest, BusyHostRejectsASecondGuest) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    CollabSession intruder;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    intruder.setTimeouts(fastTimeouts());

    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    ASSERT_TRUE(intruder.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    EXPECT_TRUE(waitFor(
            [&]() { return intruder.state() == CollabSession::State::Closed; }, 3000));
    EXPECT_TRUE(intruder.errorString().contains(QStringLiteral("already")));
    EXPECT_EQ(host.state(), CollabSession::State::Connected);
    EXPECT_EQ(guest.state(), CollabSession::State::Connected);
}

TEST(CollabSessionTest, MessagesReachThePeerAndKeepalivesStayInternal) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    int received = 0;
    CollabMessageType receivedType = CollabMessageType::Error;
    CollabClockPing receivedPing;
    QObject::connect(&host,
            &CollabSession::packetReceived,
            &host,
            [&](const CollabPacket& packet) {
                ++received;
                receivedType = packet.header.type;
                decodeCollabClockPing(packet.payload, &receivedPing);
            });

    CollabClockPing ping;
    ping.t1Us = 4242;
    ASSERT_TRUE(guest.sendMessage(
            CollabMessageType::ClockPing, encodeCollabClockPing(ping)));

    ASSERT_TRUE(waitFor([&]() { return received > 0; }, 3000));
    EXPECT_EQ(receivedType, CollabMessageType::ClockPing);
    EXPECT_EQ(receivedPing.t1Us, 4242u);

    // Let a few keepalive intervals pass: they must not surface as packets.
    waitFor([]() { return false; }, 400);
    EXPECT_EQ(received, 1);
    EXPECT_EQ(host.state(), CollabSession::State::Connected);
}

TEST(CollabSessionTest, SpoofedWelcomeFromAnotherPortIsIgnored) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    CollabSession guest;
    host.setTimeouts(fastTimeouts());
    guest.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));

    CollabSessionTimeouts longHandshake = fastTimeouts();
    longHandshake.handshakeMs = 1200;
    guest.setTimeouts(longHandshake);
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() {
                return host.state() == CollabSession::State::Connected &&
                        guest.state() == CollabSession::State::Connected;
            },
            3000));

    CollabWelcome welcome;
    welcome.sessionEpoch = 99;
    welcome.role = CollabRole::Guest;
    welcome.hostSessionTimeUs = 1234;
    CollabHeader header;
    header.type = CollabMessageType::Welcome;
    header.sessionId = makeCollabSessionId();
    header.senderPeerId = makeCollabPeerId();
    header.seq = 1;

    QUdpSocket spoof;
    ASSERT_TRUE(spoof.bind(QHostAddress::Any, 0));
    const QByteArray datagram =
            encodeCollabPacket(header, encodeCollabWelcome(welcome));
    ASSERT_EQ(spoof.writeDatagram(datagram, QHostAddress::LocalHost, guest.localPort()),
            datagram.size());

    // The real session is unaffected and the spoofed welcome was not applied.
    waitFor([]() { return false; }, 200);
    EXPECT_EQ(guest.state(), CollabSession::State::Connected);
    EXPECT_EQ(guest.sessionEpoch(), 1u);
    EXPECT_EQ(guest.remotePeerId(), host.localPeerId());
}

TEST(CollabSessionTest, ForeignSessionIdIsRejected) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabSession host;
    host.setTimeouts(fastTimeouts());
    ASSERT_TRUE(host.host(0, token));
    const CollabSessionId hostSessionId = host.sessionId();

    CollabSession guest;
    CollabSessionTimeouts longHandshake = fastTimeouts();
    longHandshake.handshakeMs = 1200;
    guest.setTimeouts(longHandshake);
    ASSERT_TRUE(guest.join(QHostAddress::LocalHost, host.localPort(), 0, token));
    ASSERT_TRUE(waitFor(
            [&]() { return guest.state() == CollabSession::State::Connected; }, 3000));

    // A HELLO that claims the same token but another session must not join.
    QUdpSocket raw;
    ASSERT_TRUE(raw.bind(QHostAddress::Any, 0));
    CollabHello hello;
    hello.role = CollabRole::Guest;
    hello.token = token;
    CollabHeader header;
    header.type = CollabMessageType::Hello;
    header.sessionId = makeCollabSessionId();
    header.senderPeerId = makeCollabPeerId();
    header.seq = 1;
    const QByteArray datagram =
            encodeCollabPacket(header, encodeCollabHello(hello));
    ASSERT_EQ(raw.writeDatagram(datagram, QHostAddress::LocalHost, host.localPort()),
            datagram.size());

    ASSERT_TRUE(waitFor(
            [&]() {
                return raw.hasPendingDatagrams() &&
                        raw.pendingDatagramSize() > 0;
            },
            2000));
    QByteArray reply;
    reply.resize(static_cast<qsizetype>(raw.pendingDatagramSize()));
    ASSERT_GT(raw.readDatagram(reply.data(), reply.size()), 0);
    CollabPacket replyPacket;
    ASSERT_TRUE(decodeCollabPacket(reply, &replyPacket));
    EXPECT_EQ(replyPacket.header.type, CollabMessageType::Reject);
    EXPECT_EQ(replyPacket.header.sessionId, header.sessionId);
    EXPECT_EQ(host.sessionId(), hostSessionId);
    EXPECT_EQ(host.remotePeerId(), guest.localPeerId());
}

TEST(CollabSessionTest, ClosedSessionCannotSend) {
    CollabSession session;
    EXPECT_FALSE(session.sendMessage(CollabMessageType::Keepalive, QByteArray()));
    EXPECT_EQ(session.lastPeerPacketAgeMs(), -1);
}

} // namespace collab
} // namespace mixxx
