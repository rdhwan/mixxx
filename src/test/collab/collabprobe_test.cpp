#include "collab/collabprobe.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QThread>
#include <QUdpSocket>

#include <functional>

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

quint16 reserveFreeUdpPort() {
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = socket.localPort();
    socket.close();
    return port;
}

} // namespace

TEST(CollabProbePacketTest, PingRoundTrip) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    ping.t1Us = 0x0102030405060708ULL;

    const QByteArray data = encodeCollabProbePacket(ping);

    CollabProbePacket decoded;
    ASSERT_TRUE(decodeCollabProbePacket(data, &decoded));
    EXPECT_EQ(decoded.type, CollabProbePacket::Type::Ping);
    EXPECT_EQ(decoded.t1Us, ping.t1Us);
    EXPECT_EQ(decoded.t2Us, 0u);
    EXPECT_EQ(decoded.t3Us, 0u);
}

TEST(CollabProbePacketTest, PongRoundTrip) {
    CollabProbePacket pong;
    pong.type = CollabProbePacket::Type::Pong;
    pong.t1Us = 100;
    pong.t2Us = 200;
    pong.t3Us = 300;

    CollabProbePacket decoded;
    ASSERT_TRUE(decodeCollabProbePacket(encodeCollabProbePacket(pong), &decoded));
    EXPECT_EQ(decoded.type, CollabProbePacket::Type::Pong);
    EXPECT_EQ(decoded.t1Us, 100u);
    EXPECT_EQ(decoded.t2Us, 200u);
    EXPECT_EQ(decoded.t3Us, 300u);
}

TEST(CollabProbePacketTest, RejectsWrongMagic) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    QByteArray data = encodeCollabProbePacket(ping);
    data[0] = 'X';

    CollabProbePacket decoded;
    EXPECT_FALSE(decodeCollabProbePacket(data, &decoded));
}

TEST(CollabProbePacketTest, RejectsWrongVersion) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    QByteArray data = encodeCollabProbePacket(ping);
    data[4] = 0x7F;

    CollabProbePacket decoded;
    EXPECT_FALSE(decodeCollabProbePacket(data, &decoded));
}

TEST(CollabProbePacketTest, RejectsWrongType) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    QByteArray data = encodeCollabProbePacket(ping);
    data[5] = 0x7F;

    CollabProbePacket decoded;
    EXPECT_FALSE(decodeCollabProbePacket(data, &decoded));
}

TEST(CollabProbePacketTest, RejectsTruncatedAndOversizedData) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    QByteArray data = encodeCollabProbePacket(ping);

    CollabProbePacket decoded;
    QByteArray truncated = data;
    truncated.chop(1);
    EXPECT_FALSE(decodeCollabProbePacket(truncated, &decoded));

    QByteArray oversized = data;
    oversized.append('\0');
    EXPECT_FALSE(decodeCollabProbePacket(oversized, &decoded));
}

TEST(CollabPlayCommandTest, RoundTrip) {
    CollabPlayCommand command;
    command.commandId = 0x1122334455667788ULL;
    command.targetSessionUs = 1234567890123LL;
    command.deck = 4;

    CollabPlayCommand decoded;
    ASSERT_TRUE(decodeCollabPlayCommand(
            encodeCollabPlayCommand(command), &decoded));
    EXPECT_EQ(decoded.commandId, command.commandId);
    EXPECT_EQ(decoded.targetSessionUs, command.targetSessionUs);
    EXPECT_EQ(decoded.deck, 4);
}

TEST(CollabPlayCommandTest, RejectsProbePacket) {
    CollabProbePacket ping;
    ping.type = CollabProbePacket::Type::Ping;
    CollabPlayCommand decoded;
    EXPECT_FALSE(decodeCollabPlayCommand(encodeCollabProbePacket(ping), &decoded));
}

TEST(CollabPlayCommandTest, RejectsWrongMagicAndSize) {
    CollabPlayCommand command;
    command.commandId = 1;
    command.targetSessionUs = 2;
    command.deck = 1;
    QByteArray data = encodeCollabPlayCommand(command);

    QByteArray wrongMagic = data;
    wrongMagic[0] = 'X';
    CollabPlayCommand decoded;
    EXPECT_FALSE(decodeCollabPlayCommand(wrongMagic, &decoded));

    QByteArray truncated = data;
    truncated.chop(1);
    EXPECT_FALSE(decodeCollabPlayCommand(truncated, &decoded));
}

TEST(CollabProbeLoopbackTest, ConnectsAndCarriesPlayCommands) {
    const quint16 portA = reserveFreeUdpPort();
    const quint16 portB = reserveFreeUdpPort();
    ASSERT_NE(portA, 0);
    ASSERT_NE(portB, 0);
    ASSERT_NE(portA, portB);

    CollabProbe probeA;
    CollabProbe probeB;
    ASSERT_TRUE(probeA.start(QStringLiteral("127.0.0.1"), portB, portA, 20));
    ASSERT_TRUE(probeB.start(QStringLiteral("127.0.0.1"), portA, portB, 20));

    EXPECT_TRUE(waitFor([&probeA]() { return probeA.isPeerConnected(); }, 3000));
    EXPECT_TRUE(waitFor([&probeB]() { return probeB.isPeerConnected(); }, 3000));
    EXPECT_TRUE(probeA.clockSync().hasEstimate());

    quint64 receivedId = 0;
    qint64 receivedTargetUs = 0;
    int receivedDeck = 0;
    QObject::connect(&probeB,
            &CollabProbe::playCommandReceived,
            &probeB,
            [&](quint64 commandId, qint64 targetSessionUs, int deck) {
                receivedId = commandId;
                receivedTargetUs = targetSessionUs;
                receivedDeck = deck;
            });

    ASSERT_TRUE(probeA.sendPlayCommand(42, 987654321, 3));
    EXPECT_TRUE(waitFor([&receivedId]() { return receivedId != 0; }, 3000));
    EXPECT_EQ(receivedId, 42u);
    EXPECT_EQ(receivedTargetUs, 987654321);
    EXPECT_EQ(receivedDeck, 3);

    probeA.stop();
    EXPECT_FALSE(probeA.isPeerConnected());
    EXPECT_TRUE(waitFor([&probeB]() { return !probeB.isPeerConnected(); }, 3000));
}

} // namespace collab
} // namespace mixxx
