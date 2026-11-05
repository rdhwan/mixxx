#include "collab/collabprobe.h"

#include <gtest/gtest.h>

#include <QByteArray>

namespace mixxx {
namespace collab {

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

} // namespace collab
} // namespace mixxx
