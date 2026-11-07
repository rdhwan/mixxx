#include "collab/collabcontrolreplicator.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <memory>
#include <vector>

#include "collab/collabprotocol.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "test/mixxxtest.h"

namespace mixxx {
namespace collab {

namespace {

const ConfigKey kPlayKey(QStringLiteral("[Channel1]"), QStringLiteral("play"));
const ConfigKey kVolumeKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"));

struct SentMessage {
    CollabMessageType type;
    QByteArray payload;
};

CollabPacket makeDeltaPacket(const QByteArray& payload) {
    CollabPacket packet;
    packet.header.type = CollabMessageType::ControlDelta;
    packet.payload = payload;
    return packet;
}

CollabPacket makeKeyframePacket(const QByteArray& payload) {
    CollabPacket packet;
    packet.header.type = CollabMessageType::ControlKeyframe;
    packet.payload = payload;
    return packet;
}

} // namespace

class CollabControlReplicatorTest : public MixxxTest {};

TEST_F(CollabControlReplicatorTest, AppliesRemoteKeyframeAndDelta) {
    ControlObject play(kPlayKey);
    ControlObject volume(kVolumeKey);

    const CollabControlMap map =
            CollabControlMap::fromKeys({kPlayKey, kVolumeKey});
    const auto playId = map.idFor(kPlayKey);
    const auto volumeId = map.idFor(kVolumeKey);
    ASSERT_TRUE(playId.has_value());
    ASSERT_TRUE(volumeId.has_value());

    std::vector<SentMessage> sent;
    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                sent.emplace_back(type, payload);
                return true;
            }));

    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = 2;
    keyframe.deltaSeq = 0;
    keyframe.values.append(CollabControlValue{*playId, 1.0});
    keyframe.values.append(CollabControlValue{*volumeId, 0.5});
    EXPECT_TRUE(replicator.handlePacket(
            makeKeyframePacket(encodeCollabControlKeyframe(keyframe))));
    EXPECT_DOUBLE_EQ(play.get(), 1.0);
    EXPECT_DOUBLE_EQ(volume.get(), 0.5);

    CollabControlDelta delta;
    delta.deltaSeq = 1;
    delta.keyframeEpoch = 2;
    delta.values.append(CollabControlValue{*volumeId, 0.25});
    EXPECT_TRUE(replicator.handlePacket(
            makeDeltaPacket(encodeCollabControlDelta(delta))));
    EXPECT_DOUBLE_EQ(volume.get(), 0.25);
    EXPECT_DOUBLE_EQ(play.get(), 1.0);
}

TEST_F(CollabControlReplicatorTest, FirstKeyframeOfASessionIsApplied) {
    ControlObject volume(kVolumeKey);

    const CollabControlMap map = CollabControlMap::fromKeys({kVolumeKey});
    const auto volumeId = map.idFor(kVolumeKey);
    ASSERT_TRUE(volumeId.has_value());

    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [](CollabMessageType, const QByteArray&) { return true; }));

    // The initial keyframe of a session carries sequence zero.
    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = 1;
    keyframe.deltaSeq = 0;
    keyframe.values.append(CollabControlValue{*volumeId, 0.6});
    EXPECT_TRUE(replicator.handlePacket(
            makeKeyframePacket(encodeCollabControlKeyframe(keyframe))));
    EXPECT_DOUBLE_EQ(volume.get(), 0.6);
}

TEST_F(CollabControlReplicatorTest, StaleDeltasAreIgnored) {
    ControlObject volume(kVolumeKey);

    const CollabControlMap map = CollabControlMap::fromKeys({kVolumeKey});
    const auto volumeId = map.idFor(kVolumeKey);
    ASSERT_TRUE(volumeId.has_value());

    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [](CollabMessageType, const QByteArray&) { return true; }));

    CollabControlDelta newer;
    newer.deltaSeq = 5;
    newer.keyframeEpoch = 1;
    newer.values.append(CollabControlValue{*volumeId, 0.8});
    ASSERT_TRUE(replicator.handlePacket(
            makeDeltaPacket(encodeCollabControlDelta(newer))));
    ASSERT_DOUBLE_EQ(volume.get(), 0.8);

    CollabControlDelta older;
    older.deltaSeq = 4;
    older.keyframeEpoch = 1;
    older.values.append(CollabControlValue{*volumeId, 0.1});
    EXPECT_TRUE(replicator.handlePacket(
            makeDeltaPacket(encodeCollabControlDelta(older))));
    EXPECT_DOUBLE_EQ(volume.get(), 0.8);
}

TEST_F(CollabControlReplicatorTest, RemoteValueIsNotEchoedBack) {
    ControlObject volume(kVolumeKey);

    const CollabControlMap map = CollabControlMap::fromKeys({kVolumeKey});
    const auto volumeId = map.idFor(kVolumeKey);
    ASSERT_TRUE(volumeId.has_value());

    std::vector<QByteArray> sent;
    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType, const QByteArray& payload) {
                sent.push_back(payload);
                return true;
            }));

    CollabControlDelta remote;
    remote.deltaSeq = 1;
    remote.keyframeEpoch = 1;
    remote.values.append(CollabControlValue{*volumeId, 0.5});
    ASSERT_TRUE(replicator.handlePacket(
            makeDeltaPacket(encodeCollabControlDelta(remote))));
    ASSERT_DOUBLE_EQ(volume.get(), 0.5);

    // An engine confirmation from another setter matches the remote value.
    volume.set(0.5);
    replicator.flush();
    EXPECT_TRUE(sent.empty());

    // A real local change is sent.
    volume.set(0.75);
    replicator.flush();
    ASSERT_EQ(sent.size(), 1u);

    CollabControlDelta decoded;
    ASSERT_TRUE(decodeCollabControlDelta(sent[0], &decoded));
    ASSERT_EQ(decoded.values.size(), 1);
    EXPECT_EQ(decoded.values[0].id, *volumeId);
    EXPECT_DOUBLE_EQ(decoded.values[0].value, 0.75);
}

TEST_F(CollabControlReplicatorTest, KeyframeContainsEveryValue) {
    ControlObject play(kPlayKey);
    ControlObject volume(kVolumeKey);
    play.set(1.0);
    volume.set(0.3);

    const CollabControlMap map =
            CollabControlMap::fromKeys({kPlayKey, kVolumeKey});

    std::vector<QByteArray> sent;
    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                EXPECT_EQ(type, CollabMessageType::ControlKeyframe);
                sent.push_back(payload);
                return true;
            }));

    ASSERT_TRUE(replicator.sendKeyframe());
    ASSERT_EQ(sent.size(), 1u);

    CollabControlKeyframe decoded;
    ASSERT_TRUE(decodeCollabControlKeyframe(sent[0], &decoded));
    EXPECT_EQ(decoded.values.size(), 2);
}

TEST_F(CollabControlReplicatorTest, UnknownRemoteIdIsReported) {
    ControlObject volume(kVolumeKey);

    const CollabControlMap map = CollabControlMap::fromKeys({kVolumeKey});

    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [](CollabMessageType, const QByteArray&) { return true; }));

    int rejected = 0;
    QObject::connect(&replicator,
            &CollabControlReplicator::packetRejected,
            &replicator,
            [&rejected](const QString&) { ++rejected; });

    CollabControlDelta delta;
    delta.deltaSeq = 1;
    delta.keyframeEpoch = 1;
    delta.values.append(CollabControlValue{999, 0.5});
    EXPECT_TRUE(replicator.handlePacket(
            makeDeltaPacket(encodeCollabControlDelta(delta))));
    EXPECT_EQ(rejected, 1);
}

TEST_F(CollabControlReplicatorTest, LocalInputOnTheOtherDeckIsReverted) {
    ControlObject ownPlay(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play")));
    ControlObject remotePlay(
            ConfigKey(QStringLiteral("[Channel3]"), QStringLiteral("play")));
    ownPlay.set(0.0);
    remotePlay.set(0.0);

    const CollabControlMap map = CollabControlMap::fromKeys(
            {ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play")),
                    ConfigKey(QStringLiteral("[Channel3]"),
                            QStringLiteral("play"))});

    std::vector<QByteArray> sent;
    CollabControlReplicator replicator;
    int blockedDeck = -1;
    QObject::connect(&replicator,
            &CollabControlReplicator::localInputBlocked,
            &replicator,
            [&blockedDeck](int deck, const QString&) { blockedDeck = deck; });

    ASSERT_TRUE(replicator.start(map, CollabDeckMap(CollabRole::Host), [&sent](CollabMessageType, const QByteArray& payload) {
                sent.push_back(payload);
                return true; }, true));

    // Deck 3 belongs to the guest on the host.
    remotePlay.set(1.0);
    EXPECT_DOUBLE_EQ(remotePlay.get(), 0.0);
    EXPECT_EQ(blockedDeck, 3);
    replicator.flush();
    EXPECT_TRUE(sent.empty());

    // Deck 1 belongs to the host and is sent normally.
    ownPlay.set(1.0);
    replicator.flush();
    ASSERT_EQ(sent.size(), 1u);
}

TEST_F(CollabControlReplicatorTest, GuestCannotMoveTheCrossfader) {
    ControlObject crossfader(
            ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
    crossfader.set(0.0);

    const CollabControlMap map = CollabControlMap::fromKeys(
            {ConfigKey(QStringLiteral("[Master]"),
                    QStringLiteral("crossfader"))});

    CollabControlReplicator replicator;
    int blockedDeck = -1;
    QObject::connect(&replicator,
            &CollabControlReplicator::localInputBlocked,
            &replicator,
            [&blockedDeck](int deck, const QString&) { blockedDeck = deck; });

    ASSERT_TRUE(replicator.start(map, CollabDeckMap(CollabRole::Guest), [](CollabMessageType, const QByteArray&) { return true; }, true));

    crossfader.set(1.0);
    EXPECT_DOUBLE_EQ(crossfader.get(), 0.0);
    EXPECT_EQ(blockedDeck, 0);
}

TEST_F(CollabControlReplicatorTest, BlockedControlCascadeTerminates) {
    const ConfigKey volumeKey(
            QStringLiteral("[Channel3]"), QStringLiteral("volume"));
    ControlObject remoteVolume(volumeKey);
    remoteVolume.set(0.0);

    // Emulate an engine correction that writes the control from its own
    // value-changed handler, the way loop start/end clamping does. Every
    // value except 1.0 is rejected and replaced by 1.0.
    ControlProxy rejecter(volumeKey);
    rejecter.connectValueChanged(&rejecter, [&remoteVolume](double value) {
        if (value != 1.0) {
            remoteVolume.set(1.0);
        }
    });

    const CollabControlMap map = CollabControlMap::fromKeys({volumeKey});
    const auto volumeId = map.idFor(volumeKey);
    ASSERT_TRUE(volumeId.has_value());

    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map, CollabDeckMap(CollabRole::Host), [](CollabMessageType, const QByteArray&) { return true; }, true));

    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = 1;
    keyframe.deltaSeq = 0;
    keyframe.values.append(CollabControlValue{*volumeId, 2.0});
    EXPECT_TRUE(replicator.handlePacket(
            makeKeyframePacket(encodeCollabControlKeyframe(keyframe))));
    // The engine correction wins. The point of the test is that applying it
    // does not recurse through the ownership revert.
    EXPECT_DOUBLE_EQ(remoteVolume.get(), 1.0);
}

TEST_F(CollabControlReplicatorTest, LoopPointsAreAppliedBeforeEnabling) {
    const ConfigKey startKey(
            QStringLiteral("[Channel1]"), QStringLiteral("loop_start_position"));
    const ConfigKey endKey(
            QStringLiteral("[Channel1]"), QStringLiteral("loop_end_position"));
    const ConfigKey enabledKey(
            QStringLiteral("[Channel1]"), QStringLiteral("loop_enabled"));
    ControlObject loopStart(startKey);
    ControlObject loopEnd(endKey);
    ControlObject loopEnabled(enabledKey);
    loopStart.set(-1.0);
    loopEnd.set(-1.0);
    loopEnabled.set(0.0);

    // Emulate LoopingControl: enabling a loop is only confirmed when both
    // points are valid, and the end must be after the start.
    loopEnabled.connectValueChangeRequest(
            &loopEnabled, [&loopStart, &loopEnd, &loopEnabled](double value) {
                if (value > 0.0) {
                    if (loopStart.get() >= 0.0 &&
                            loopEnd.get() > loopStart.get()) {
                        loopEnabled.setAndConfirm(1.0);
                    }
                } else {
                    loopEnabled.setAndConfirm(0.0);
                }
            });

    const CollabControlMap map =
            CollabControlMap::fromKeys({startKey, endKey, enabledKey});
    const auto startId = map.idFor(startKey);
    const auto endId = map.idFor(endKey);
    const auto enabledId = map.idFor(enabledKey);
    ASSERT_TRUE(startId.has_value());
    ASSERT_TRUE(endId.has_value());
    ASSERT_TRUE(enabledId.has_value());

    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [](CollabMessageType, const QByteArray&) { return true; }));

    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = 1;
    // The map's order puts loop_enabled before the points, and a delta may
    // arrive in any order. Send the worst case on purpose.
    keyframe.values.append(CollabControlValue{*enabledId, 1.0});
    keyframe.values.append(CollabControlValue{*endId, 88000.0});
    keyframe.values.append(CollabControlValue{*startId, 44000.0});

    ASSERT_TRUE(replicator.handlePacket(
            makeKeyframePacket(encodeCollabControlKeyframe(keyframe))));
    EXPECT_DOUBLE_EQ(loopStart.get(), 44000.0);
    EXPECT_DOUBLE_EQ(loopEnd.get(), 88000.0);
    EXPECT_DOUBLE_EQ(loopEnabled.get(), 1.0);
}

TEST_F(CollabControlReplicatorTest, KeyframeIsSplitAcrossDatagrams) {
    QList<ConfigKey> keys;
    std::vector<std::unique_ptr<ControlObject>> controls;
    constexpr int kControlCount = 200;
    for (int i = 0; i < kControlCount; ++i) {
        const ConfigKey key(QStringLiteral("[Channel1]"),
                QStringLiteral("collab_test_control_%1").arg(i));
        keys.append(key);
        controls.push_back(std::make_unique<ControlObject>(key));
    }

    const CollabControlMap map = CollabControlMap::fromKeys(keys);
    ASSERT_EQ(map.size(), kControlCount);

    std::vector<CollabControlKeyframe> sent;
    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                EXPECT_EQ(type, CollabMessageType::ControlKeyframe);
                CollabControlKeyframe keyframe;
                if (decodeCollabControlKeyframe(payload, &keyframe)) {
                    sent.push_back(keyframe);
                    return true;
                }
                return false;
            }));

    ASSERT_TRUE(replicator.sendKeyframe());
    ASSERT_EQ(sent.size(), 3u);

    std::size_t totalValues = 0;
    uint32_t lastSeq = 0;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        const CollabControlKeyframe& keyframe = sent[i];
        EXPECT_EQ(keyframe.chunkIndex, i);
        EXPECT_EQ(keyframe.chunkCount, sent.size());
        EXPECT_LE(keyframe.values.size(), kCollabMaxControlsPerPacket);
        EXPECT_GT(keyframe.deltaSeq, lastSeq);
        lastSeq = keyframe.deltaSeq;
        totalValues += static_cast<std::size_t>(keyframe.values.size());
    }
    EXPECT_EQ(totalValues, static_cast<std::size_t>(kControlCount));
}

TEST_F(CollabControlReplicatorTest, MomentaryTriggerSkipsTheFlushWindow) {
    const ConfigKey cueKey(
            QStringLiteral("[Channel1]"), QStringLiteral("cue_default"));
    ControlObject cue(cueKey);

    const CollabControlMap map = CollabControlMap::fromKeys({cueKey});
    const auto cueId = map.idFor(cueKey);
    ASSERT_TRUE(cueId.has_value());

    std::vector<CollabControlDelta> sent;
    CollabControlReplicator replicator;
    ASSERT_TRUE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [&sent](CollabMessageType type, const QByteArray& payload) {
                EXPECT_EQ(type, CollabMessageType::ControlDelta);
                CollabControlDelta delta;
                if (decodeCollabControlDelta(payload, &delta)) {
                    sent.push_back(delta);
                    return true;
                }
                return false;
            }));

    cue.set(1.0);
    ASSERT_EQ(sent.size(), 1u);
    ASSERT_EQ(sent.back().values.size(), 1);
    EXPECT_EQ(sent.back().values.front().id, *cueId);
    EXPECT_DOUBLE_EQ(sent.back().values.front().value, 1.0);

    // The release must arrive as its own delta, not be coalesced away.
    cue.set(0.0);
    ASSERT_EQ(sent.size(), 2u);
    ASSERT_EQ(sent.back().values.size(), 1);
    EXPECT_DOUBLE_EQ(sent.back().values.front().value, 0.0);
}

TEST_F(CollabControlReplicatorTest, StartFailsForAMissingControl) {
    const CollabControlMap map = CollabControlMap::fromKeys(
            {ConfigKey(QStringLiteral("[Channel1]"),
                    QStringLiteral("not_a_real_control"))});

    CollabControlReplicator replicator;
    EXPECT_FALSE(replicator.start(map,
            CollabDeckMap(CollabRole::Host),
            [](CollabMessageType, const QByteArray&) { return true; }));
    EXPECT_FALSE(replicator.isRunning());
    EXPECT_FALSE(replicator.errorString().isEmpty());
}

} // namespace collab
} // namespace mixxx
