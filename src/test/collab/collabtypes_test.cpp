#include "collab/collabtypes.h"

#include <gtest/gtest.h>

namespace mixxx {
namespace collab {

TEST(CollabTypesTest, PeerIdHexRoundTrip) {
    const CollabPeerId peerId = {0x00, 0x11, 0x22, 0x33, 0xaa, 0xbb, 0xcc, 0xff};
    const QString hex = peerIdToHex(peerId);
    EXPECT_EQ(hex, QStringLiteral("00112233aabbccff"));

    CollabPeerId parsed{};
    EXPECT_TRUE(peerIdFromHex(hex, &parsed));
    EXPECT_EQ(parsed, peerId);
}

TEST(CollabTypesTest, SessionIdHexRoundTrip) {
    const CollabSessionId sessionId = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const QString hex = sessionIdToHex(sessionId);
    EXPECT_EQ(hex, QStringLiteral("000102030405060708090a0b0c0d0e0f"));

    CollabSessionId parsed{};
    EXPECT_TRUE(sessionIdFromHex(hex, &parsed));
    EXPECT_EQ(parsed, sessionId);
}

TEST(CollabTypesTest, InvalidHexIsRejected) {
    CollabPeerId peerId{};
    EXPECT_FALSE(peerIdFromHex(QStringLiteral(""), &peerId));
    EXPECT_FALSE(peerIdFromHex(QStringLiteral("0011"), &peerId));
    EXPECT_FALSE(peerIdFromHex(QStringLiteral("00112233aabbccfz"), &peerId));
    EXPECT_FALSE(peerIdFromHex(QStringLiteral("00112233aabbccff00"), &peerId));
}

TEST(CollabTypesTest, DeckIdBounds) {
    EXPECT_FALSE(isValidCollabDeckId(0));
    EXPECT_TRUE(isValidCollabDeckId(1));
    EXPECT_TRUE(isValidCollabDeckId(4));
    EXPECT_FALSE(isValidCollabDeckId(5));
}

} // namespace collab
} // namespace mixxx
