#include "collab/deckstatereplicator.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>
#include <vector>

#include "collab/collabprotocol.h"
#include "control/controlobject.h"
#include "test/mixxxtest.h"

namespace mixxx {
namespace collab {

namespace {

constexpr double kDurationSeconds = 200.0;
constexpr double kSampleRate = 44100.0;

struct DeckControls {
    explicit DeckControls(const QString& group = QStringLiteral("[Channel1]"))
            : play(ConfigKey(group, QStringLiteral("play"))),
              playposition(ConfigKey(group, QStringLiteral("playposition"))),
              duration(ConfigKey(group, QStringLiteral("duration"))),
              trackSamples(ConfigKey(group, QStringLiteral("track_samples"))),
              rateRatio(ConfigKey(group, QStringLiteral("rate_ratio"))),
              reverse(ConfigKey(group, QStringLiteral("reverse"))),
              loopEnabled(ConfigKey(group, QStringLiteral("loop_enabled"))),
              loopStart(ConfigKey(group, QStringLiteral("loop_start_position"))),
              loopEnd(ConfigKey(group, QStringLiteral("loop_end_position"))) {
    }

    void load(double durationSeconds = kDurationSeconds) {
        duration.set(durationSeconds);
        trackSamples.set(durationSeconds * kSampleRate);
        playposition.set(0.0);
        rateRatio.set(1.0);
        reverse.set(0.0);
        play.set(0.0);
        loopEnabled.set(0.0);
        loopStart.set(0.0);
        loopEnd.set(0.0);
    }

    ControlObject play;
    ControlObject playposition;
    ControlObject duration;
    ControlObject trackSamples;
    ControlObject rateRatio;
    ControlObject reverse;
    ControlObject loopEnabled;
    ControlObject loopStart;
    ControlObject loopEnd;
};

CollabPacket makeAnchorPacket(const CollabDeckAnchor& anchor) {
    CollabPacket packet;
    packet.header.type = CollabMessageType::DeckAnchor;
    packet.payload = encodeCollabDeckAnchor(anchor);
    return packet;
}

} // namespace

class DeckStateReplicatorTest : public MixxxTest {};

TEST_F(DeckStateReplicatorTest, OwnerAnchorCarriesPlayheadState) {
    DeckControls controls;
    controls.load();
    controls.playposition.set(0.25);
    controls.rateRatio.set(1.1);
    controls.reverse.set(1.0);
    controls.play.set(1.0);
    controls.loopEnabled.set(1.0);
    const double trackSamples = kDurationSeconds * kSampleRate;
    controls.loopStart.set(trackSamples * 0.1);
    controls.loopEnd.set(trackSamples * 0.2);

    std::vector<CollabDeckAnchor> sent;
    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                EXPECT_EQ(type, CollabMessageType::DeckAnchor);
                CollabDeckAnchor anchor;
                if (decodeCollabDeckAnchor(payload, &anchor)) {
                    sent.push_back(anchor);
                    return true;
                }
                return false;
            },
            []() { return 5000000LL; },
            []() { return true; }));

    ASSERT_TRUE(replicator.sendAnchor(1));
    ASSERT_EQ(sent.size(), 1u);
    const CollabDeckAnchor& anchor = sent.front();
    EXPECT_EQ(anchor.deckId, 1);
    EXPECT_EQ(anchor.stateVersion, 1u);
    EXPECT_DOUBLE_EQ(anchor.durationSeconds, kDurationSeconds);
    EXPECT_DOUBLE_EQ(anchor.position, 0.25);
    EXPECT_DOUBLE_EQ(anchor.rate, -1.1);
    EXPECT_TRUE(anchor.playing);
    EXPECT_TRUE(anchor.loopEnabled);
    EXPECT_NEAR(anchor.loopStart, 0.1, 1e-9);
    EXPECT_NEAR(anchor.loopEnd, 0.2, 1e-9);
    EXPECT_EQ(anchor.anchorSessionTimeUs, 5000000u);
}

TEST_F(DeckStateReplicatorTest, SlowScratchIsForwarded) {
    DeckControls controls;
    controls.load();
    controls.play.set(0.0);

    std::vector<CollabDeckAnchor> sent;
    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                EXPECT_EQ(type, CollabMessageType::DeckAnchor);
                CollabDeckAnchor anchor;
                if (decodeCollabDeckAnchor(payload, &anchor)) {
                    sent.push_back(anchor);
                    return true;
                }
                return false;
            },
            []() { return 5000000LL; },
            []() { return true; }));

    // A scratch reaches the owner as many small per-buffer increments. Keep
    // every single step below the threshold; the accumulated movement must
    // still be forwarded.
    const double step = 0.006 / kDurationSeconds;
    controls.playposition.set(step);
    EXPECT_TRUE(sent.empty());
    controls.playposition.set(2.0 * step);
    EXPECT_TRUE(sent.empty());
    controls.playposition.set(3.0 * step);
    EXPECT_TRUE(sent.empty());
    controls.playposition.set(4.0 * step);

    ASSERT_EQ(sent.size(), 1u);
    EXPECT_NEAR(sent.front().position, 4.0 * step, 1e-12);
    EXPECT_FALSE(sent.front().playing);
}

TEST_F(DeckStateReplicatorTest, SteadyPlaybackDoesNotSendExtraAnchors) {
    DeckControls controls;
    controls.load();
    controls.play.set(1.0);

    qint64 nowUs = 5000000;
    std::vector<CollabDeckAnchor> sent;
    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType, const QByteArray& payload) {
                CollabDeckAnchor anchor;
                if (decodeCollabDeckAnchor(payload, &anchor)) {
                    sent.push_back(anchor);
                    return true;
                }
                return false;
            },
            [&nowUs]() { return nowUs; },
            []() { return true; }));

    double position = 0.0;
    const double step = 0.010 / kDurationSeconds;
    for (int update = 0; update < 100; ++update) {
        nowUs += 10000;
        position += step;
        controls.playposition.set(position);
    }

    // The playhead advanced exactly as the rate predicts, so no seek or
    // scratch anchor is due. Only the periodic timer sends during playback.
    EXPECT_TRUE(sent.empty());
}

TEST_F(DeckStateReplicatorTest, StateVersionAdvancesAcrossRestarts) {
    DeckControls controls;
    controls.load();
    controls.play.set(0.0);

    std::vector<uint32_t> versions;
    auto send = [&versions](CollabMessageType, const QByteArray& payload) {
        CollabDeckAnchor anchor;
        if (decodeCollabDeckAnchor(payload, &anchor)) {
            versions.push_back(anchor.stateVersion);
        }
        return true;
    };
    auto sessionNow = []() { return 5000000LL; };
    auto ready = []() { return true; };

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Host), send, sessionNow, ready));
    ASSERT_TRUE(replicator.sendAnchor(1));
    replicator.stop();

    // A reconnect restarts the replicator while the follower may still be
    // running. The version must keep increasing or the follower discards the
    // fresh anchors as stale.
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Host), send, sessionNow, ready));
    ASSERT_TRUE(replicator.sendAnchor(1));

    ASSERT_EQ(versions.size(), 2u);
    EXPECT_GT(versions.back(), versions.front());
}

TEST_F(DeckStateReplicatorTest, FollowerSeeksToAPausedAnchor) {
    DeckControls controls;
    controls.load();

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Guest),
            [](CollabMessageType, const QByteArray&) { return true; },
            []() { return 5000000LL; },
            []() { return true; }));

    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.stateVersion = 1;
    anchor.durationSeconds = kDurationSeconds;
    anchor.position = 0.5;
    anchor.rate = 1.0;
    anchor.playing = false;
    anchor.anchorSessionTimeUs = 5000000;

    EXPECT_TRUE(replicator.handlePacket(makeAnchorPacket(anchor)));
    EXPECT_NEAR(controls.playposition.get(), 0.5, 1e-9);
    EXPECT_GT(replicator.lastCorrectionMs(1), 1000.0);
}

TEST_F(DeckStateReplicatorTest, FollowerCorrectsPlayingDrift) {
    DeckControls controls;
    controls.load();
    controls.playposition.set(0.5);

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Guest),
            [](CollabMessageType, const QByteArray&) { return true; },
            []() { return 5020000LL; },
            []() { return true; }));
    double lastErrorMs = 0.0;
    QObject::connect(&replicator,
            &DeckStateReplicator::correctionApplied,
            [&lastErrorMs](int, double errorMs) { lastErrorMs = errorMs; });

    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.stateVersion = 1;
    anchor.durationSeconds = kDurationSeconds;
    anchor.position = 0.5;
    anchor.rate = 1.0;
    anchor.playing = true;
    anchor.anchorSessionTimeUs = 5000000;

    EXPECT_TRUE(replicator.handlePacket(makeAnchorPacket(anchor)));
    // The anchor is 20 ms old, so the playhead should have advanced by
    // 0.020 / 200 in normalized position.
    EXPECT_NEAR(controls.playposition.get(), 0.5 + 0.020 / kDurationSeconds, 1e-9);
    EXPECT_NEAR(lastErrorMs, 20.0, 0.5);
}

TEST_F(DeckStateReplicatorTest, FollowerIgnoresASuppressedAnchor) {
    DeckControls controls;
    controls.load();
    controls.playposition.set(0.75);

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Guest),
            [](CollabMessageType, const QByteArray&) { return true; },
            []() { return 5000000LL; },
            []() { return true; }));

    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.stateVersion = 7;
    anchor.durationSeconds = kDurationSeconds;
    anchor.position = 0.75;
    anchor.rate = 1.0;
    anchor.playing = false;
    anchor.anchorSessionTimeUs = 5000000;
    EXPECT_TRUE(replicator.handlePacket(makeAnchorPacket(anchor)));
    EXPECT_NEAR(controls.playposition.get(), 0.75, 1e-9);
    EXPECT_NEAR(replicator.lastCorrectionMs(1), 0.0, 1e-6);
}

TEST_F(DeckStateReplicatorTest, FollowerWaitsForTheClock) {
    DeckControls controls;
    controls.load();

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Guest),
            [](CollabMessageType, const QByteArray&) { return true; },
            []() { return 5000000LL; },
            []() { return false; }));

    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.stateVersion = 1;
    anchor.durationSeconds = kDurationSeconds;
    anchor.position = 0.5;
    anchor.rate = 1.0;
    anchor.playing = false;
    anchor.anchorSessionTimeUs = 5000000;

    EXPECT_TRUE(replicator.handlePacket(makeAnchorPacket(anchor)));
    EXPECT_NEAR(controls.playposition.get(), 0.0, 1e-9);
}

TEST_F(DeckStateReplicatorTest, FollowerIgnoresADifferentTrack) {
    DeckControls controls;
    controls.load();

    DeckStateReplicator replicator;
    ASSERT_TRUE(replicator.start(
            CollabDeckMap(CollabRole::Guest),
            [](CollabMessageType, const QByteArray&) { return true; },
            []() { return 5000000LL; },
            []() { return true; }));

    CollabDeckAnchor anchor;
    anchor.deckId = 1;
    anchor.stateVersion = 1;
    anchor.durationSeconds = kDurationSeconds * 2.0;
    anchor.position = 0.5;
    anchor.rate = 1.0;
    anchor.playing = false;
    anchor.anchorSessionTimeUs = 5000000;

    EXPECT_TRUE(replicator.handlePacket(makeAnchorPacket(anchor)));
    EXPECT_NEAR(controls.playposition.get(), 0.0, 1e-9);
}

} // namespace collab
} // namespace mixxx
