#include "collab/clocksync.h"

#include <gtest/gtest.h>

#include <memory>

#include "collab/collabclock.h"

namespace mixxx {
namespace collab {

TEST(CollabClockSyncTest, SymmetricExchangeYieldsExactOffset) {
    CollabClockSync sync;
    // Remote is 5000 us ahead. 2 ms of path delay each way.
    ASSERT_TRUE(sync.addSample(100000, 106000, 106000, 102000));

    EXPECT_TRUE(sync.hasEstimate());
    EXPECT_EQ(sync.offsetUs(), 5000);
    EXPECT_EQ(sync.rttUs(), 2000);
    EXPECT_EQ(sync.skewPpm(), 0.0);
}

TEST(CollabClockSyncTest, NegativeOffsetIsPreserved) {
    CollabClockSync sync;
    // Remote is 8000 us behind local.
    ASSERT_TRUE(sync.addSample(200000, 193000, 193050, 202050));

    EXPECT_EQ(sync.offsetUs(), -8000 + 0);
}

TEST(CollabClockSyncTest, InconsistentTimestampsAreRejected) {
    CollabClockSync sync;

    EXPECT_FALSE(sync.addSample(100, 100, 100, 90));
    EXPECT_FALSE(sync.addSample(100, 100, 90, 200));
    EXPECT_FALSE(sync.hasEstimate());
    EXPECT_EQ(sync.sampleCount(), 0);
}

TEST(CollabClockSyncTest, HighRttOutlierDoesNotMoveOffset) {
    CollabClockSync sync;
    // Base estimate: +5000 us with a 2 ms RTT.
    for (int i = 0; i < 6; ++i) {
        const qint64 t1 = 100000 + i * 10000;
        ASSERT_TRUE(sync.addSample(t1, t1 + 5000 + 1000, t1 + 5000 + 1100, t1 + 2100));
    }
    EXPECT_EQ(sync.offsetUs(), 5000);

    // A heavily delayed exchange with a biased offset.
    const qint64 t1 = 1000000;
    ASSERT_TRUE(sync.addSample(t1, t1 + 50000, t1 + 50000, t1 + 200000));

    EXPECT_EQ(sync.offsetUs(), 5000);
    EXPECT_LT(sync.acceptedSampleCount(), sync.sampleCount());
}

TEST(CollabClockSyncTest, MedianRejectsSingleBiasedSample) {
    CollabClockSync sync;
    const qint64 base = 500000;
    // Four consistent samples with +5000 us offset and 2 ms RTT.
    for (int i = 0; i < 4; ++i) {
        const qint64 t1 = base + i * 1000;
        ASSERT_TRUE(sync.addSample(t1, t1 + 6000, t1 + 6050, t1 + 2050));
    }
    // One sample with the same RTT but a different offset.
    ASSERT_TRUE(sync.addSample(base + 5000, base + 5000 + 9000, base + 5000 + 9050, base + 5000 + 2050));

    EXPECT_EQ(sync.offsetUs(), 5000);
}

TEST(CollabClockSyncTest, SkewIsEstimatedFromDriftingOffsets) {
    CollabClockSync sync;
    // Remote clock runs 100 ppm fast: offset grows by 100 us per second.
    const qint64 startUs = 1000000;
    for (int i = 0; i < 30; ++i) {
        const qint64 t1 = startUs + i * 1000000;
        const qint64 remoteTime = t1 + 1000 + 2000 + (t1 - startUs) / 10000;
        ASSERT_TRUE(sync.addSample(t1, remoteTime, remoteTime + 100, t1 + 2100));
    }

    EXPECT_NEAR(sync.skewPpm(), 100.0, 2.0);
    // After one second the offset used for conversion should have grown.
    EXPECT_NEAR(sync.toRemoteTime(startUs + 1000000) -
                    (startUs + 1000000) - 2000,
            100,
            3);
}

TEST(CollabClockSyncTest, ImplausibleSkewIsIgnored) {
    CollabClockSync sync;
    const qint64 startUs = 1000000;
    for (int i = 0; i < 20; ++i) {
        const qint64 t1 = startUs + i * 1000000;
        // 50 000 ppm is not a plausible audio clock.
        const qint64 offset = 5000 + (t1 - startUs) / 20;
        ASSERT_TRUE(sync.addSample(t1, t1 + offset + 1000, t1 + offset + 1100, t1 + 2100));
    }

    EXPECT_EQ(sync.skewPpm(), 0.0);
}

TEST(CollabClockSyncTest, ConversionRoundTrips) {
    CollabClockSync sync;
    const qint64 startUs = 2000000;
    for (int i = 0; i < 20; ++i) {
        const qint64 t1 = startUs + i * 1000000;
        const qint64 offset = 7000 + (t1 - startUs) / 10000;
        ASSERT_TRUE(sync.addSample(t1, t1 + offset + 1000, t1 + offset + 1100, t1 + 2100));
    }

    const qint64 localUs = startUs + 1500000;
    const qint64 remoteUs = sync.toRemoteTime(localUs);
    EXPECT_NEAR(sync.toLocalTime(remoteUs), localUs, 2);
}

TEST(CollabClockSyncTest, ResetClearsEstimate) {
    CollabClockSync sync;
    ASSERT_TRUE(sync.addSample(1000, 2000, 2100, 1200));

    sync.reset();

    EXPECT_FALSE(sync.hasEstimate());
    EXPECT_EQ(sync.sampleCount(), 0);
    EXPECT_EQ(sync.offsetUs(), 0);
    EXPECT_EQ(sync.toRemoteTime(1234), 1234);
}

namespace {
// Base pattern: remote is 5000 us ahead, 2 ms RTT, symmetric path delay.
bool addBaseSample(CollabClockSync& sync, qint64 t1) {
    return sync.addSample(t1, t1 + 6000, t1 + 6000, t1 + 2000);
}
} // namespace

TEST(CollabClockSyncTest, SmallJitterDoesNotReportAJump) {
    CollabClockSync sync;
    ASSERT_TRUE(addBaseSample(sync, 100000));
    for (int i = 1; i < 5; ++i) {
        const qint64 t1 = 100000 + i * 10000;
        // +-5 ms of offset noise stays far below the 30 ms jump threshold.
        const qint64 noise = (i % 2 == 0) ? 5000 : -5000;
        ASSERT_TRUE(sync.addSample(t1,
                t1 + 6000 + noise,
                t1 + 6000 + noise,
                t1 + 2000));
        EXPECT_EQ(sync.takeLastJumpUs(), 0);
    }
}

TEST(CollabClockSyncTest, ClockStepReAnchorsAndReportsTheJump) {
    CollabClockSync sync;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(addBaseSample(sync, 100000 + i * 10000));
    }
    ASSERT_TRUE(sync.hasEstimate());
    EXPECT_EQ(sync.offsetUs(), 5000);

    // The peer's clock steps 200 ms forward.
    const qint64 t1 = 300000;
    const qint64 stepUs = 200000;
    ASSERT_TRUE(sync.addSample(t1,
            t1 + 6000 + stepUs,
            t1 + 6000 + stepUs,
            t1 + 2000));

    EXPECT_EQ(sync.takeLastJumpUs(), stepUs);
    // The estimate follows the new offset immediately instead of being held
    // back by the pre-jump history.
    EXPECT_EQ(sync.offsetUs(), 5000 + stepUs);
    // The jump is consumed.
    EXPECT_EQ(sync.takeLastJumpUs(), 0);
}

TEST(CollabClockSyncTest, NegativeClockStepReAnchorsAndReportsTheJump) {
    CollabClockSync sync;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(addBaseSample(sync, 100000 + i * 10000));
    }
    const qint64 t1 = 300000;
    const qint64 stepUs = -50000;
    ASSERT_TRUE(sync.addSample(t1,
            t1 + 6000 + stepUs,
            t1 + 6000 + stepUs,
            t1 + 2000));
    EXPECT_EQ(sync.takeLastJumpUs(), stepUs);
    EXPECT_EQ(sync.offsetUs(), 5000 + stepUs);
}

TEST(CollabClockTest, ManualClockAdvances) {
    ManualCollabClock clock(1000);
    EXPECT_EQ(clock.nowUs(), 1000);

    clock.advanceUs(500);
    EXPECT_EQ(clock.nowUs(), 1500);

    clock.setNowUs(42);
    EXPECT_EQ(clock.nowUs(), 42);
}

TEST(CollabClockTest, SkewedClockAppliesOffsetAndSkew) {
    auto pBase = std::make_unique<ManualCollabClock>(1000000);
    ManualCollabClock* pManual = pBase.get();
    SkewedCollabClock clock(5000, 100.0, std::move(pBase));

    EXPECT_EQ(clock.nowUs(), 1000000 + 5000 + 100);

    pManual->advanceUs(1000000);
    EXPECT_EQ(clock.nowUs(), 2000000 + 5000 + 200);
}

TEST(CollabSessionClockTest, MapsLocalTimeToSessionTime) {
    ManualCollabClock clock(1000000);
    CollabClockSync sync;
    ASSERT_TRUE(sync.addSample(1000000, 1004000, 1004000, 1002000));

    const CollabSessionClock sessionClock(&clock, &sync);

    EXPECT_EQ(sessionClock.localNowUs(), 1000000);
    EXPECT_EQ(sessionClock.sessionNowUs(), 1003000);
    EXPECT_EQ(sessionClock.sessionToLocalUs(1003000), 1000000);
}

} // namespace collab
} // namespace mixxx
