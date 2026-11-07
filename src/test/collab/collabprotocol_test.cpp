#include "collab/collabprotocol.h"

#include <gtest/gtest.h>

#include <QByteArray>

namespace mixxx {
namespace collab {

namespace {

CollabHeader makeHeader() {
    CollabHeader header;
    header.type = CollabMessageType::Command;
    header.flags = CollabFlagReliable | CollabFlagRequestAck;
    for (std::size_t i = 0; i < header.sessionId.size(); ++i) {
        header.sessionId[i] = static_cast<uint8_t>(i);
    }
    for (std::size_t i = 0; i < header.senderPeerId.size(); ++i) {
        header.senderPeerId[i] = static_cast<uint8_t>(0xA0 + i);
    }
    header.seq = 0x11223344;
    header.ack = 0x55667788;
    header.ackBits = 0x0102030405060708ULL;
    header.sendTimeUs = 0xDEADBEEFCAFEULL;
    return header;
}

} // namespace

TEST(CollabProtocolHeaderTest, RoundTrip) {
    const CollabHeader header = makeHeader();
    const QByteArray payload = QByteArrayLiteral("\x01\x02\x00\xFF");

    const QByteArray data = encodeCollabPacket(header, payload);
    ASSERT_EQ(data.size(), kCollabHeaderSize + payload.size());

    CollabPacket decoded;
    ASSERT_TRUE(decodeCollabPacket(data, &decoded));
    EXPECT_EQ(decoded.header.type, header.type);
    EXPECT_EQ(decoded.header.flags, header.flags);
    EXPECT_EQ(decoded.header.sessionId, header.sessionId);
    EXPECT_EQ(decoded.header.senderPeerId, header.senderPeerId);
    EXPECT_EQ(decoded.header.seq, header.seq);
    EXPECT_EQ(decoded.header.ack, header.ack);
    EXPECT_EQ(decoded.header.ackBits, header.ackBits);
    EXPECT_EQ(decoded.header.sendTimeUs, header.sendTimeUs);
    EXPECT_EQ(decoded.payload, payload);
}

TEST(CollabProtocolHeaderTest, EmptyPayloadIsValid) {
    CollabPacket decoded;
    ASSERT_TRUE(decodeCollabPacket(encodeCollabPacket(makeHeader(), QByteArray()), &decoded));
    EXPECT_TRUE(decoded.payload.isEmpty());
}

TEST(CollabProtocolHeaderTest, RejectsForeignMagic) {
    QByteArray data = encodeCollabPacket(makeHeader(), QByteArray());
    data[0] = 'X';
    CollabPacket decoded;
    EXPECT_FALSE(decodeCollabPacket(data, &decoded));
}

TEST(CollabProtocolHeaderTest, RejectsWrongVersion) {
    QByteArray data = encodeCollabPacket(makeHeader(), QByteArray());
    data[4] = static_cast<char>(kCollabProtocolVersion + 1);
    CollabPacket decoded;
    EXPECT_FALSE(decodeCollabPacket(data, &decoded));
}

TEST(CollabProtocolHeaderTest, RejectsTruncatedHeader) {
    QByteArray data = encodeCollabPacket(makeHeader(), QByteArray());
    data.chop(1);
    CollabPacket decoded;
    EXPECT_FALSE(decodeCollabPacket(data, &decoded));
}

TEST(CollabProtocolHeaderTest, RejectsOversizedDatagram) {
    QByteArray data = encodeCollabPacket(makeHeader(), QByteArray());
    data.append(QByteArray(kCollabMaxDatagramSize, '\0'));
    CollabPacket decoded;
    EXPECT_FALSE(decodeCollabPacket(data, &decoded));
}

TEST(CollabProtocolHelloTest, RoundTrip) {
    CollabHello hello;
    hello.role = CollabRole::Guest;
    hello.featureFlags = 0x0000000F;
    for (std::size_t i = 0; i < hello.token.size(); ++i) {
        hello.token[i] = static_cast<uint8_t>(i * 3);
    }

    CollabHello decoded;
    ASSERT_TRUE(decodeCollabHello(encodeCollabHello(hello), &decoded));
    EXPECT_EQ(decoded.role, CollabRole::Guest);
    EXPECT_EQ(decoded.featureFlags, hello.featureFlags);
    EXPECT_EQ(decoded.token, hello.token);
}

TEST(CollabProtocolHelloTest, RejectsInvalidRole) {
    QByteArray payload = encodeCollabHello(CollabHello{});
    payload[0] = 2;
    CollabHello decoded;
    EXPECT_FALSE(decodeCollabHello(payload, &decoded));
}

TEST(CollabProtocolWelcomeTest, RoundTrip) {
    CollabWelcome welcome;
    welcome.sessionEpoch = 7;
    welcome.role = CollabRole::Guest;
    welcome.hostSessionTimeUs = 123456789012ULL;

    CollabWelcome decoded;
    ASSERT_TRUE(decodeCollabWelcome(encodeCollabWelcome(welcome), &decoded));
    EXPECT_EQ(decoded.sessionEpoch, 7u);
    EXPECT_EQ(decoded.role, CollabRole::Guest);
    EXPECT_EQ(decoded.hostSessionTimeUs, welcome.hostSessionTimeUs);
}

TEST(CollabProtocolByeTest, RoundTrip) {
    CollabBye bye;
    bye.reasonCode = 42;
    bye.message = QStringLiteral("host is shutting down");

    CollabBye decoded;
    ASSERT_TRUE(decodeCollabBye(encodeCollabBye(bye), &decoded));
    EXPECT_EQ(decoded.reasonCode, 42);
    EXPECT_EQ(decoded.message, bye.message);
}

TEST(CollabProtocolByeTest, TruncatesLongMessage) {
    CollabBye bye;
    bye.message = QString(4000, QLatin1Char('x'));

    CollabBye decoded;
    ASSERT_TRUE(decodeCollabBye(encodeCollabBye(bye), &decoded));
    EXPECT_EQ(decoded.message.size(), 512);
}

TEST(CollabProtocolClockTest, PingPongRoundTrip) {
    CollabClockPing ping;
    ping.t1Us = 111;
    CollabClockPing pingOut;
    ASSERT_TRUE(decodeCollabClockPing(encodeCollabClockPing(ping), &pingOut));
    EXPECT_EQ(pingOut.t1Us, 111u);

    CollabClockPong pong;
    pong.t1Us = 111;
    pong.t2Us = 222;
    pong.t3Us = 333;
    CollabClockPong pongOut;
    ASSERT_TRUE(decodeCollabClockPong(encodeCollabClockPong(pong), &pongOut));
    EXPECT_EQ(pongOut.t1Us, 111u);
    EXPECT_EQ(pongOut.t2Us, 222u);
    EXPECT_EQ(pongOut.t3Us, 333u);
}

TEST(CollabProtocolCommandTest, RoundTrip) {
    CollabCommand command;
    command.commandId = 0x1122334455667788ULL;
    command.action = CollabCommandAction::Seek;
    command.flags = 1;
    command.deckId = 3;
    command.targetSessionTimeUs = 9876543210ULL;
    ASSERT_TRUE(command.addArg(CollabArgPositionFrames, 44100.0 * 12.5));

    CollabCommand decoded;
    ASSERT_TRUE(decodeCollabCommand(encodeCollabCommand(command), &decoded));
    EXPECT_EQ(decoded.commandId, command.commandId);
    EXPECT_EQ(decoded.action, CollabCommandAction::Seek);
    EXPECT_EQ(decoded.flags, 1);
    EXPECT_EQ(decoded.deckId, 3);
    EXPECT_EQ(decoded.targetSessionTimeUs, command.targetSessionTimeUs);
    ASSERT_EQ(decoded.argCount, 1);
    EXPECT_EQ(decoded.args[0].id, CollabArgPositionFrames);
    EXPECT_DOUBLE_EQ(decoded.args[0].value, 44100.0 * 12.5);
}

TEST(CollabProtocolCommandTest, ArgLimitIsEnforced) {
    CollabCommand command;
    for (int i = 0; i < kCollabMaxCommandArgs; ++i) {
        EXPECT_TRUE(command.addArg(static_cast<uint16_t>(i + 1), i));
    }
    EXPECT_FALSE(command.addArg(99, 0.0));
    EXPECT_EQ(command.argCount, kCollabMaxCommandArgs);
}

TEST(CollabProtocolCommandTest, RejectsMissingDeck) {
    CollabCommand command;
    command.deckId = 0;
    CollabCommand decoded;
    EXPECT_FALSE(decodeCollabCommand(encodeCollabCommand(command), &decoded));
}

TEST(CollabProtocolCommandTest, RejectsDeckOutOfRange) {
    CollabCommand command;
    command.deckId = 5;
    CollabCommand decoded;
    EXPECT_FALSE(decodeCollabCommand(encodeCollabCommand(command), &decoded));
}

TEST(CollabProtocolCommandAckTest, RoundTrip) {
    CollabCommandAck ack;
    ack.commandId = 9;
    ack.result = CollabCommandResult::Late;
    ack.appliedSessionTimeUs = 555;
    ack.reasonCode = 2;
    ack.reason = QStringLiteral("missed the target");

    CollabCommandAck decoded;
    ASSERT_TRUE(decodeCollabCommandAck(encodeCollabCommandAck(ack), &decoded));
    EXPECT_EQ(decoded.commandId, 9u);
    EXPECT_EQ(decoded.result, CollabCommandResult::Late);
    EXPECT_EQ(decoded.appliedSessionTimeUs, 555u);
    EXPECT_EQ(decoded.reasonCode, 2);
    EXPECT_EQ(decoded.reason, ack.reason);
}

TEST(CollabProtocolTokenTest, ComparisonIsExact) {
    std::array<uint8_t, 16> a{};
    std::array<uint8_t, 16> b{};
    EXPECT_TRUE(collabTokensEqual(a, b));
    b[15] = 1;
    EXPECT_FALSE(collabTokensEqual(a, b));
}

TEST(CollabProtocolRandomTest, IdsDiffer) {
    const CollabSessionId sessionA = makeCollabSessionId();
    const CollabSessionId sessionB = makeCollabSessionId();
    EXPECT_NE(sessionA, sessionB);

    const CollabPeerId peerA = makeCollabPeerId();
    const CollabPeerId peerB = makeCollabPeerId();
    EXPECT_NE(peerA, peerB);

    const CollabSessionId zero{};
    EXPECT_NE(sessionA, zero);
}

TEST(CollabProtocolTokenTest, CodeDerivationIsCaseAndSpaceInsensitive) {
    const CollabSessionToken upper = collabTokenFromCode(QStringLiteral("ABC234"));
    const CollabSessionToken spaced = collabTokenFromCode(QStringLiteral("  abc234 "));
    EXPECT_TRUE(collabTokensEqual(upper, spaced));

    const CollabSessionToken other = collabTokenFromCode(QStringLiteral("ABC235"));
    EXPECT_FALSE(collabTokensEqual(upper, other));
    EXPECT_NE(upper, CollabSessionToken{});
}

TEST(CollabProtocolTokenTest, GeneratedCodeUsesTheReadableAlphabet) {
    const QString code = makeCollabSessionCode();
    ASSERT_EQ(code.size(), 6);
    for (const QChar character : code) {
        EXPECT_TRUE(character.isLetterOrNumber());
        EXPECT_FALSE(QStringLiteral("01OI").contains(character));
    }
}

} // namespace collab
} // namespace mixxx
