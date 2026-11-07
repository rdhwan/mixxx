#include "collab/collabseqtracker.h"

#include <gtest/gtest.h>

namespace mixxx {
namespace collab {

TEST(CollabSequenceTrackerTest, InOrderPacketsAreNotLost) {
    CollabSequenceTracker tracker;
    for (uint32_t seq = 1; seq <= 100; ++seq) {
        tracker.note(seq);
    }
    EXPECT_EQ(tracker.receivedCount(), 100u);
    EXPECT_EQ(tracker.lostCount(), 0u);
    EXPECT_DOUBLE_EQ(tracker.lossPercent(), 0.0);
}

TEST(CollabSequenceTrackerTest, GapIsCountedImmediately) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(4);
    EXPECT_EQ(tracker.receivedCount(), 2u);
    EXPECT_EQ(tracker.lostCount(), 2u);
    EXPECT_DOUBLE_EQ(tracker.lossPercent(), 50.0);
}

TEST(CollabSequenceTrackerTest, ReorderedPacketClearsTheLoss) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(4);
    EXPECT_EQ(tracker.lostCount(), 2u);

    tracker.note(2);
    tracker.note(3);
    EXPECT_EQ(tracker.receivedCount(), 4u);
    EXPECT_EQ(tracker.lostCount(), 0u);
}

TEST(CollabSequenceTrackerTest, DuplicateIsIgnored) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(2);
    tracker.note(2);
    tracker.note(1);
    EXPECT_EQ(tracker.receivedCount(), 2u);
    EXPECT_EQ(tracker.lostCount(), 0u);
}

TEST(CollabSequenceTrackerTest, PacketOlderThanTheWindowStaysLost) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(100);
    // 98 packets are missing; only the last 63 can still be recovered.
    EXPECT_EQ(tracker.lostCount(), 98u);

    tracker.note(2);
    EXPECT_EQ(tracker.lostCount(), 98u);
    EXPECT_EQ(tracker.receivedCount(), 2u);
}

TEST(CollabSequenceTrackerTest, LatePacketInsideTheWindowIsRecovered) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(100);
    tracker.note(50);
    EXPECT_EQ(tracker.lostCount(), 97u);
    EXPECT_EQ(tracker.receivedCount(), 3u);
}

TEST(CollabSequenceTrackerTest, ZeroSequenceIsIgnored) {
    CollabSequenceTracker tracker;
    tracker.note(0);
    EXPECT_EQ(tracker.receivedCount(), 0u);
    EXPECT_EQ(tracker.lostCount(), 0u);
    EXPECT_DOUBLE_EQ(tracker.lossPercent(), 0.0);
}

TEST(CollabSequenceTrackerTest, ResetClearsEverything) {
    CollabSequenceTracker tracker;
    tracker.note(1);
    tracker.note(5);
    tracker.reset();
    EXPECT_EQ(tracker.receivedCount(), 0u);
    EXPECT_EQ(tracker.lostCount(), 0u);

    tracker.note(1);
    EXPECT_EQ(tracker.receivedCount(), 1u);
    EXPECT_EQ(tracker.lostCount(), 0u);
}

} // namespace collab
} // namespace mixxx
