#include "collab/collabcontrolreplicator.h"

#include <gtest/gtest.h>

#include <QByteArray>
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
