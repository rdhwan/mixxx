#include "collab/commandscheduler.h"

#include <gtest/gtest.h>

#include <vector>

namespace mixxx {
namespace collab {

namespace {

struct ExecutedCommand {
    CollabCommandScheduler::CommandId id;
    qint64 latenessUs;
};

} // namespace

TEST(CollabCommandSchedulerTest, ExecutesOnlyWhenDue) {
    CollabCommandScheduler scheduler;
    std::vector<ExecutedCommand> executed;
    const auto callback = [&executed](CollabCommandScheduler::CommandId id,
                                  qint64 latenessUs) {
        executed.push_back(ExecutedCommand{id, latenessUs});
    };

    ASSERT_TRUE(scheduler.schedule(1, 10000, callback));
    EXPECT_EQ(scheduler.runDue(9999), 0);
    EXPECT_EQ(executed.size(), 0);

    EXPECT_EQ(scheduler.runDue(10000), 1);
    ASSERT_EQ(executed.size(), 1);
    EXPECT_EQ(executed[0].id, 1);
    EXPECT_EQ(executed[0].latenessUs, 0);
    EXPECT_EQ(scheduler.pendingCount(), 0);
}

TEST(CollabCommandSchedulerTest, ExecutesInTargetOrder) {
    CollabCommandScheduler scheduler;
    std::vector<CollabCommandScheduler::CommandId> order;
    const auto callback = [&order](CollabCommandScheduler::CommandId id, qint64) {
        order.push_back(id);
    };

    ASSERT_TRUE(scheduler.schedule(1, 20000, callback));
    ASSERT_TRUE(scheduler.schedule(2, 10000, callback));
    ASSERT_TRUE(scheduler.schedule(3, 15000, callback));

    EXPECT_EQ(scheduler.runDue(25000), 3);
    EXPECT_EQ(order, (std::vector<CollabCommandScheduler::CommandId>{2, 3, 1}));
}

TEST(CollabCommandSchedulerTest, ReportsLateness) {
    CollabCommandScheduler scheduler;
    qint64 lateness = -1;
    ASSERT_TRUE(scheduler.schedule(7, 10000, [&lateness](auto, qint64 late) {
        lateness = late;
    }));

    EXPECT_EQ(scheduler.runDue(10250), 1);
    EXPECT_EQ(lateness, 250);
}

TEST(CollabCommandSchedulerTest, DropsCommandsBeyondTolerance) {
    CollabCommandScheduler scheduler;
    scheduler.setMaxLatenessUs(1000);
    bool called = false;
    ASSERT_TRUE(scheduler.schedule(7, 10000, [&called](auto, qint64) {
        called = true;
    }));

    EXPECT_EQ(scheduler.runDue(20000), 0);
    EXPECT_FALSE(called);
    // The identifier is remembered so a retransmission is not applied late.
    EXPECT_TRUE(scheduler.isKnown(7));
}

TEST(CollabCommandSchedulerTest, DuplicateIdentifierIsRejected) {
    CollabCommandScheduler scheduler;
    int calls = 0;
    const auto callback = [&calls](auto, qint64) {
        ++calls;
    };

    ASSERT_TRUE(scheduler.schedule(5, 1000, callback));
    EXPECT_FALSE(scheduler.schedule(5, 2000, callback));
    EXPECT_EQ(scheduler.pendingCount(), 1);

    EXPECT_EQ(scheduler.runDue(3000), 1);
    EXPECT_EQ(calls, 1);

    // A retransmitted command after execution is acknowledged, not reapplied.
    EXPECT_TRUE(scheduler.isKnown(5));
    EXPECT_FALSE(scheduler.schedule(5, 3000, callback));
    EXPECT_EQ(scheduler.runDue(4000), 0);
    EXPECT_EQ(calls, 1);
}

TEST(CollabCommandSchedulerTest, CancelRemovesPendingCommand) {
    CollabCommandScheduler scheduler;
    bool called = false;
    ASSERT_TRUE(scheduler.schedule(3, 1000, [&called](auto, qint64) {
        called = true;
    }));

    EXPECT_TRUE(scheduler.cancel(3));
    EXPECT_FALSE(scheduler.cancel(3));
    EXPECT_EQ(scheduler.pendingCount(), 0);
    EXPECT_EQ(scheduler.runDue(5000), 0);
    EXPECT_FALSE(called);
}

TEST(CollabCommandSchedulerTest, InvalidCommandsAreRejected) {
    CollabCommandScheduler scheduler;

    EXPECT_FALSE(scheduler.schedule(0, 1000, [](auto, qint64) {}));
    EXPECT_FALSE(scheduler.schedule(1, -1, [](auto, qint64) {}));
    EXPECT_FALSE(scheduler.schedule(2, 1000, CollabCommandScheduler::Callback()));
    EXPECT_EQ(scheduler.pendingCount(), 0);
}

TEST(CollabCommandSchedulerTest, ClearForgetsPendingAndHistory) {
    CollabCommandScheduler scheduler;
    ASSERT_TRUE(scheduler.schedule(1, 1000, [](auto, qint64) {}));
    scheduler.runDue(2000);

    scheduler.clear();

    EXPECT_EQ(scheduler.pendingCount(), 0);
    EXPECT_FALSE(scheduler.isKnown(1));
}

TEST(CollabCommandSchedulerTest, CountsExecutedAndDroppedCommands) {
    CollabCommandScheduler scheduler;
    scheduler.setMaxLatenessUs(1000);
    ASSERT_TRUE(scheduler.schedule(1, 10000, [](auto, qint64) {}));

    EXPECT_EQ(scheduler.runDue(10500), 1);
    EXPECT_EQ(scheduler.executedCount(), 1u);
    EXPECT_EQ(scheduler.droppedCount(), 0u);
    EXPECT_EQ(scheduler.maxObservedLatenessUs(), 500);

    ASSERT_TRUE(scheduler.schedule(3, 10000, [](auto, qint64) {}));
    EXPECT_EQ(scheduler.runDue(20000), 0);
    EXPECT_EQ(scheduler.executedCount(), 1u);
    EXPECT_EQ(scheduler.droppedCount(), 1u);
    EXPECT_EQ(scheduler.maxObservedLatenessUs(), 500);
}

TEST(CollabCommandSchedulerTest, ResetStatisticsKeepsPendingCommands) {
    CollabCommandScheduler scheduler;
    ASSERT_TRUE(scheduler.schedule(1, 1000, [](auto, qint64) {}));
    scheduler.runDue(2000);
    ASSERT_TRUE(scheduler.schedule(2, 5000, [](auto, qint64) {}));
    ASSERT_EQ(scheduler.pendingCount(), 1);

    scheduler.resetStatistics();

    EXPECT_EQ(scheduler.executedCount(), 0u);
    EXPECT_EQ(scheduler.droppedCount(), 0u);
    EXPECT_EQ(scheduler.maxObservedLatenessUs(), 0);
    EXPECT_EQ(scheduler.pendingCount(), 1);
}

TEST(CollabCommandSchedulerTest, TargetFromLeadAddsLead) {
    EXPECT_EQ(CollabCommandScheduler::targetFromLead(1000, 60000), 61000);
}

TEST(CollabCommandSchedulerTest, ShiftMovesPendingTargetsAndKeepsOrder) {
    CollabCommandScheduler scheduler;
    std::vector<ExecutedCommand> executed;
    const auto callback = [&executed](CollabCommandScheduler::CommandId id,
                                  qint64 latenessUs) {
        executed.push_back(ExecutedCommand{id, latenessUs});
    };

    ASSERT_TRUE(scheduler.schedule(1, 20000, callback));
    ASSERT_TRUE(scheduler.schedule(2, 30000, callback));

    scheduler.shiftPendingTargets(5000);
    EXPECT_EQ(scheduler.runDue(24000), 0);
    EXPECT_EQ(scheduler.runDue(25000), 1);
    ASSERT_EQ(executed.size(), 1);
    EXPECT_EQ(executed[0].id, 1);
    EXPECT_EQ(executed[0].latenessUs, 0);

    // A negative shift that lands a target in the past fires with lateness.
    // 30000 + 5000 - 10000 = 25000; fired at 26000 -> 1000 us late.
    scheduler.shiftPendingTargets(-10000);
    EXPECT_EQ(scheduler.runDue(26000), 1);
    ASSERT_EQ(executed.size(), 2);
    EXPECT_EQ(executed[1].id, 2);
    EXPECT_EQ(executed[1].latenessUs, 1000);
}

TEST(CollabCommandSchedulerTest, ShiftClampsTargetsToZero) {
    CollabCommandScheduler scheduler;
    std::vector<CollabCommandScheduler::CommandId> dropped;
    const auto callback = [&dropped](CollabCommandScheduler::CommandId id,
                                  qint64 latenessUs) {
        dropped.push_back(id);
    };
    ASSERT_TRUE(scheduler.schedule(1, 1000, callback));
    scheduler.setMaxLatenessUs(100000);

    scheduler.shiftPendingTargets(-5000);
    EXPECT_EQ(scheduler.pendingCount(), 1);
    EXPECT_EQ(scheduler.runDue(1), 1);
    EXPECT_EQ(dropped.size(), 1u);
}

} // namespace collab
} // namespace mixxx
