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

TEST(CollabProtocolControlDeltaTest, RoundTrip) {
    CollabControlDelta delta;
    delta.deltaSeq = 42;
    delta.keyframeEpoch = 3;
    delta.values.append(CollabControlValue{7, 0.25});
    delta.values.append(CollabControlValue{9, -1.5});

    CollabControlDelta decoded;
    ASSERT_TRUE(decodeCollabControlDelta(encodeCollabControlDelta(delta), &decoded));
    EXPECT_EQ(decoded.deltaSeq, 42u);
    EXPECT_EQ(decoded.keyframeEpoch, 3u);
    ASSERT_EQ(decoded.values.size(), 2);
    EXPECT_EQ(decoded.values[0].id, 7);
    EXPECT_DOUBLE_EQ(decoded.values[0].value, 0.25);
    EXPECT_EQ(decoded.values[1].id, 9);
    EXPECT_DOUBLE_EQ(decoded.values[1].value, -1.5);
}

TEST(CollabProtocolControlDeltaTest, RejectsTooManyValues) {
    CollabControlDelta delta;
    for (int i = 0; i < kCollabMaxControlsPerPacket + 1; ++i) {
        delta.values.append(
                CollabControlValue{static_cast<CollabControlId>(i + 1), 0.0});
    }
    // Encoding truncates to the maximum; the datagram itself stays decodable.
    CollabControlDelta decoded;
    ASSERT_TRUE(decodeCollabControlDelta(encodeCollabControlDelta(delta), &decoded));
    EXPECT_EQ(decoded.values.size(), kCollabMaxControlsPerPacket);
}

TEST(CollabProtocolControlDeltaTest, RejectsInvalidId) {
    CollabControlDelta delta;
    delta.values.append(CollabControlValue{kInvalidCollabControlId, 1.0});
    CollabControlDelta decoded;
    EXPECT_FALSE(decodeCollabControlDelta(encodeCollabControlDelta(delta), &decoded));
}

TEST(CollabProtocolControlDeltaTest, RejectsTrailingData) {
    QByteArray payload = encodeCollabControlDelta(CollabControlDelta{});
    payload.append('\0');
    CollabControlDelta decoded;
    EXPECT_FALSE(decodeCollabControlDelta(payload, &decoded));
}

TEST(CollabProtocolControlKeyframeTest, RoundTrip) {
    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = 5;
    keyframe.deltaSeq = 77;
    keyframe.chunkIndex = 1;
    keyframe.chunkCount = 3;
    keyframe.sessionTimeUs = 123456789;
    keyframe.values.append(CollabControlValue{1, 1.0});

    CollabControlKeyframe decoded;
    ASSERT_TRUE(
            decodeCollabControlKeyframe(encodeCollabControlKeyframe(keyframe), &decoded));
    EXPECT_EQ(decoded.keyframeEpoch, 5u);
    EXPECT_EQ(decoded.deltaSeq, 77u);
    EXPECT_EQ(decoded.chunkIndex, 1);
    EXPECT_EQ(decoded.chunkCount, 3);
    EXPECT_EQ(decoded.sessionTimeUs, 123456789u);
    ASSERT_EQ(decoded.values.size(), 1);
    EXPECT_DOUBLE_EQ(decoded.values[0].value, 1.0);
}

TEST(CollabProtocolControlKeyframeTest, RejectsInvalidChunk) {
    CollabControlKeyframe keyframe;
    keyframe.chunkIndex = 3;
    keyframe.chunkCount = 3;
    CollabControlKeyframe decoded;
    EXPECT_FALSE(
            decodeCollabControlKeyframe(encodeCollabControlKeyframe(keyframe), &decoded));

    keyframe.chunkIndex = 0;
    keyframe.chunkCount = 0;
    EXPECT_FALSE(
            decodeCollabControlKeyframe(encodeCollabControlKeyframe(keyframe), &decoded));
}

TEST(CollabProtocolDeckAnchorTest, RoundTrip) {
    CollabDeckAnchor anchor;
    anchor.deckId = 3;
    anchor.stateVersion = 42;
    anchor.durationSeconds = 240.5;
    anchor.position = 0.375;
    anchor.rate = 1.25;
    anchor.playing = true;
    anchor.loopEnabled = true;
    anchor.loopStart = 0.25;
    anchor.loopEnd = 0.5;
    anchor.anchorSessionTimeUs = 987654321;

    CollabDeckAnchor decoded;
    ASSERT_TRUE(
            decodeCollabDeckAnchor(encodeCollabDeckAnchor(anchor), &decoded));
    EXPECT_EQ(decoded.deckId, 3);
    EXPECT_EQ(decoded.stateVersion, 42u);
    EXPECT_DOUBLE_EQ(decoded.durationSeconds, 240.5);
    EXPECT_DOUBLE_EQ(decoded.position, 0.375);
    EXPECT_DOUBLE_EQ(decoded.rate, 1.25);
    EXPECT_TRUE(decoded.playing);
    EXPECT_TRUE(decoded.loopEnabled);
    EXPECT_DOUBLE_EQ(decoded.loopStart, 0.25);
    EXPECT_DOUBLE_EQ(decoded.loopEnd, 0.5);
    EXPECT_EQ(decoded.anchorSessionTimeUs, 987654321u);
}

TEST(CollabProtocolDeckAnchorTest, RejectsInvalidDeckAndPosition) {
    CollabDeckAnchor anchor;
    anchor.deckId = 0;
    anchor.durationSeconds = 1.0;
    anchor.position = 0.5;
    CollabDeckAnchor decoded;
    EXPECT_FALSE(
            decodeCollabDeckAnchor(encodeCollabDeckAnchor(anchor), &decoded));

    anchor.deckId = 1;
    anchor.position = 1.5;
    EXPECT_FALSE(
            decodeCollabDeckAnchor(encodeCollabDeckAnchor(anchor), &decoded));
}

TEST(CollabProtocolDeckAnchorTest, RejectsTrailingData) {
    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.durationSeconds = 1.0;
    anchor.position = 0.5;
    QByteArray payload = encodeCollabDeckAnchor(anchor);
    payload.append('\0');
    CollabDeckAnchor decoded;
    EXPECT_FALSE(decodeCollabDeckAnchor(payload, &decoded));
}

namespace {

CollabContentHash makeTrackHash(uint8_t seed) {
    CollabContentHash hash;
    hash.bytes = QByteArray(CollabContentHash::kSize, static_cast<char>(seed));
    return hash;
}

} // namespace

TEST(CollabProtocolLoadRequestTest, RoundTrip) {
    CollabLoadRequest request;
    request.deckId = 3;
    request.track.hash = makeTrackHash(0x2a);
    request.track.size = 123456789;
    request.track.suffix = QStringLiteral("flac");
    request.track.title = QStringLiteral("Track — Ünïcode");
    request.track.transferPort = 40420;
    request.requireAnalysis = true;
    CollabContentHash analysisHash;
    analysisHash.bytes = makeTrackHash(0x5b).bytes;
    request.analysisHash = analysisHash;

    const QByteArray payload = encodeCollabLoadRequest(request);
    CollabLoadRequest decoded;
    ASSERT_TRUE(decodeCollabLoadRequest(payload, &decoded));
    EXPECT_EQ(decoded.deckId, request.deckId);
    EXPECT_EQ(decoded.track.hash, request.track.hash);
    EXPECT_EQ(decoded.track.size, request.track.size);
    EXPECT_EQ(decoded.track.suffix, request.track.suffix);
    EXPECT_EQ(decoded.track.title, request.track.title);
    EXPECT_EQ(decoded.track.transferPort, request.track.transferPort);
    EXPECT_TRUE(decoded.requireAnalysis);
    EXPECT_EQ(decoded.analysisHash, request.analysisHash);
}

TEST(CollabProtocolLoadRequestTest, RejectsInvalidFields) {
    CollabLoadRequest request;
    request.deckId = 1;
    request.track.hash = makeTrackHash(1);
    request.track.size = 10;
    request.track.transferPort = 40420;

    CollabLoadRequest decoded;

    CollabLoadRequest badDeck = request;
    badDeck.deckId = 0;
    EXPECT_FALSE(decodeCollabLoadRequest(
            encodeCollabLoadRequest(badDeck), &decoded));
    badDeck.deckId = 5;
    EXPECT_FALSE(decodeCollabLoadRequest(
            encodeCollabLoadRequest(badDeck), &decoded));

    CollabLoadRequest noPort = request;
    noPort.track.transferPort = 0;
    EXPECT_FALSE(decodeCollabLoadRequest(
            encodeCollabLoadRequest(noPort), &decoded));

    QByteArray trailing = encodeCollabLoadRequest(request);
    trailing.append('\0');
    EXPECT_FALSE(decodeCollabLoadRequest(trailing, &decoded));

    QByteArray truncated = encodeCollabLoadRequest(request);
    truncated.chop(1);
    EXPECT_FALSE(decodeCollabLoadRequest(truncated, &decoded));
}

TEST(CollabProtocolTrackNeedTest, RoundTrip) {
    CollabTrackNeed need;
    need.hash = makeTrackHash(0x55);

    CollabTrackNeed decoded;
    ASSERT_TRUE(decodeCollabTrackNeed(encodeCollabTrackNeed(need), &decoded));
    EXPECT_EQ(decoded.hash, need.hash);

    QByteArray shortPayload = encodeCollabTrackNeed(need);
    shortPayload.chop(1);
    EXPECT_FALSE(decodeCollabTrackNeed(shortPayload, &decoded));
}

TEST(CollabProtocolLoadReplyTest, RoundTrip) {
    CollabLoadReply reply;
    reply.deckId = 4;
    reply.hash = makeTrackHash(0x77);
    reply.status = CollabLoadStatus::Failed;
    reply.reasonCode = 3;
    reply.reason = QStringLiteral("not enough disk space");

    CollabLoadReply decoded;
    ASSERT_TRUE(decodeCollabLoadReply(encodeCollabLoadReply(reply), &decoded));
    EXPECT_EQ(decoded.deckId, reply.deckId);
    EXPECT_EQ(decoded.hash, reply.hash);
    EXPECT_EQ(decoded.status, reply.status);
    EXPECT_EQ(decoded.reasonCode, reply.reasonCode);
    EXPECT_EQ(decoded.reason, reply.reason);

    reply.status = CollabLoadStatus::Ready;
    reply.reason.clear();
    ASSERT_TRUE(decodeCollabLoadReply(encodeCollabLoadReply(reply), &decoded));
    EXPECT_EQ(decoded.status, CollabLoadStatus::Ready);
    EXPECT_TRUE(decoded.reason.isEmpty());
}

TEST(CollabProtocolLoadReplyTest, RejectsInvalidStatusAndTrailingData) {
    CollabLoadReply reply;
    reply.deckId = 2;
    reply.hash = makeTrackHash(0x11);

    QByteArray payload = encodeCollabLoadReply(reply);
    // The status byte follows the deck id (2 bytes) and the hash (32 bytes).
    payload[34] = 2;
    CollabLoadReply decoded;
    EXPECT_FALSE(decodeCollabLoadReply(payload, &decoded));

    QByteArray trailing = encodeCollabLoadReply(reply);
    trailing.append('\0');
    EXPECT_FALSE(decodeCollabLoadReply(trailing, &decoded));
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
