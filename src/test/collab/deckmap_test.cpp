#include "collab/deckmap.h"

#include <gtest/gtest.h>

namespace mixxx {
namespace collab {

namespace {

constexpr int kLeftOrientation = 0;
constexpr int kRightOrientation = 2;

} // namespace

TEST(CollabDeckMapTest, HostMapsSessionDecksOneToOne) {
    const CollabDeckMap deckMap(CollabRole::Host);

    for (int deck = 1; deck <= kCollabSessionDeckCount; ++deck) {
        EXPECT_EQ(deckMap.localDeckForSessionDeck(deck), deck);
        EXPECT_EQ(deckMap.sessionDeckForLocalDeck(deck), deck);
    }

    EXPECT_TRUE(deckMap.ownsSessionDeck(1));
    EXPECT_TRUE(deckMap.ownsSessionDeck(2));
    EXPECT_FALSE(deckMap.ownsSessionDeck(3));
    EXPECT_FALSE(deckMap.ownsSessionDeck(4));
}

TEST(CollabDeckMapTest, GuestMapsOwnDecksToLocalOneAndTwo) {
    const CollabDeckMap deckMap(CollabRole::Guest);

    EXPECT_EQ(deckMap.localDeckForSessionDeck(1), 3);
    EXPECT_EQ(deckMap.localDeckForSessionDeck(2), 4);
    EXPECT_EQ(deckMap.localDeckForSessionDeck(3), 1);
    EXPECT_EQ(deckMap.localDeckForSessionDeck(4), 2);

    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(1), 3);
    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(2), 4);
    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(3), 1);
    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(4), 2);

    EXPECT_FALSE(deckMap.ownsSessionDeck(1));
    EXPECT_FALSE(deckMap.ownsSessionDeck(2));
    EXPECT_TRUE(deckMap.ownsSessionDeck(3));
    EXPECT_TRUE(deckMap.ownsSessionDeck(4));
}

TEST(CollabDeckMapTest, InvalidDecksAreRejected) {
    const CollabDeckMap deckMap(CollabRole::Host);

    EXPECT_EQ(deckMap.localDeckForSessionDeck(0), kInvalidCollabDeckId);
    EXPECT_EQ(deckMap.localDeckForSessionDeck(5), kInvalidCollabDeckId);
    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(0), kInvalidCollabDeckId);
    EXPECT_EQ(deckMap.sessionDeckForLocalDeck(5), kInvalidCollabDeckId);
    EXPECT_EQ(deckMap.crossfaderOrientationForSessionDeck(0), -1);
    EXPECT_EQ(deckMap.localDeckIndexForSessionDeck(0), -1);
    EXPECT_TRUE(deckMap.localGroupForSessionDeck(0).isEmpty());
}

TEST(CollabDeckMapTest, CrossfaderOrientationFollowsSessionDeck) {
    const CollabDeckMap host(CollabRole::Host);
    const CollabDeckMap guest(CollabRole::Guest);

    EXPECT_EQ(host.crossfaderOrientationForSessionDeck(1), kLeftOrientation);
    EXPECT_EQ(host.crossfaderOrientationForSessionDeck(2), kRightOrientation);
    EXPECT_EQ(host.crossfaderOrientationForSessionDeck(3), kLeftOrientation);
    EXPECT_EQ(host.crossfaderOrientationForSessionDeck(4), kRightOrientation);

    EXPECT_EQ(guest.crossfaderOrientationForSessionDeck(1), kLeftOrientation);
    EXPECT_EQ(guest.crossfaderOrientationForSessionDeck(2), kRightOrientation);
    EXPECT_EQ(guest.crossfaderOrientationForSessionDeck(3), kLeftOrientation);
    EXPECT_EQ(guest.crossfaderOrientationForSessionDeck(4), kRightOrientation);
}

TEST(CollabDeckMapTest, LocalGroupUsesMappedDeck) {
    const CollabDeckMap host(CollabRole::Host);
    const CollabDeckMap guest(CollabRole::Guest);

    EXPECT_EQ(host.localGroupForSessionDeck(3), QStringLiteral("[Channel3]"));
    EXPECT_EQ(guest.localGroupForSessionDeck(3), QStringLiteral("[Channel1]"));
    EXPECT_EQ(guest.localGroupForSessionDeck(4), QStringLiteral("[Channel2]"));
}

TEST(CollabDeckMapTest, LocalDeckIndexIsZeroBased) {
    const CollabDeckMap guest(CollabRole::Guest);

    EXPECT_EQ(guest.localDeckIndexForSessionDeck(3), 0);
    EXPECT_EQ(guest.localDeckIndexForSessionDeck(4), 1);
    EXPECT_EQ(guest.localDeckIndexForSessionDeck(1), 2);
    EXPECT_EQ(guest.localDeckIndexForSessionDeck(2), 3);
}

} // namespace collab
} // namespace mixxx
