#include "collab/collabconnection.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QThread>
#include <algorithm>
#include <functional>

#include "collab/collabprotocol.h"
#include "control/controlobject.h"
#include "test/mixxxtest.h"

namespace mixxx {
namespace collab {

namespace {

bool waitFor(const std::function<bool()>& predicate, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return predicate();
}

CollabSessionTimeouts fastConnectionTimeouts() {
    CollabSessionTimeouts timeouts;
    timeouts.handshakeMs = 2000;
    timeouts.handshakeRetryMs = 50;
    timeouts.keepaliveMs = 100;
    timeouts.peerTimeoutMs = 500;
    return timeouts;
}

QList<ConfigKey> testControlKeys() {
    return {ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Channel3]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")),
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("orientation"))};
}

} // namespace

class CollabConnectionTest : public MixxxTest {};

TEST_F(CollabConnectionTest, HostAndGuestSynchroniseTheirClocks) {
    const CollabSessionToken token = collabTokenFromCode(QStringLiteral("TEST42"));

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
    // This test has no engine controls; the replicator has its own tests.
    host.setControlReplicationEnabled(false);
    guest.setControlReplicationEnabled(false);

    ASSERT_TRUE(host.host(0, token, QString()));
    ASSERT_NE(host.localPort(), 0);
    ASSERT_TRUE(guest.join(
            QStringLiteral("127.0.0.1"), host.localPort(), 0, token, QString()));

    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));
    EXPECT_TRUE(host.isHost());
    EXPECT_FALSE(guest.isHost());
    // The host sees the guest through a dual-stack socket; the address shown
    // in the UI must stay readable.
    EXPECT_TRUE(host.peerAddress().contains(QStringLiteral("127.0.0.1")));
    EXPECT_FALSE(host.peerAddress().contains(QStringLiteral("::ffff:")));
    EXPECT_TRUE(host.isActive());
    EXPECT_EQ(host.state(), CollabSession::State::Connected);

    ASSERT_TRUE(waitFor([&]() { return guest.hasClockEstimate(); }, 5000));
    EXPECT_GT(guest.sampleCount(), 0);
    EXPECT_LT(qAbs(guest.offsetUs()), 50 * 1000);

    // The loopback path is short, so both peers see a small round trip.
    EXPECT_LT(host.rttUs(), 20 * 1000);
    EXPECT_GE(guest.lastResponseAgeMs(), 0);
}

TEST_F(CollabConnectionTest, LeavingEndsTheOtherSide) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
    host.setControlReplicationEnabled(false);
    guest.setControlReplicationEnabled(false);
    ASSERT_TRUE(host.host(0, token, QString()));
    ASSERT_TRUE(guest.join(
            QStringLiteral("127.0.0.1"), host.localPort(), 0, token, QString()));
    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));

    guest.leave();
    EXPECT_FALSE(guest.isActive());
    EXPECT_TRUE(waitFor([&]() { return !host.isActive(); }, 5000));
    EXPECT_EQ(host.state(), CollabSession::State::Closed);
    EXPECT_FALSE(host.errorString().isEmpty());
}

TEST_F(CollabConnectionTest, ReplicatesControlsOverLoopback) {
    ControlObject hostVolume(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")));
    ControlObject guestVolume(ConfigKey(QStringLiteral("[Channel3]"), QStringLiteral("volume")));
    ControlObject crossfader(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
    ControlObject orientation(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("orientation")));
    hostVolume.set(0.3);

    const CollabSessionToken token = collabTokenFromCode(QStringLiteral("TEST42"));
    const QList<ConfigKey> keys = testControlKeys();

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
    host.setControlKeys(keys);
    guest.setControlKeys(keys);

    uint32_t guestKeyframeEpoch = 0;
    QObject::connect(&guest,
            &CollabConnection::controlKeyframeApplied,
            [&guestKeyframeEpoch](uint32_t epoch) {
                guestKeyframeEpoch = epoch;
            });

    ASSERT_TRUE(host.host(0, token, QString()));
    ASSERT_TRUE(guest.join(
            QStringLiteral("127.0.0.1"), host.localPort(), 0, token, QString()));
    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));

    // The host's keyframe reached the guest over the real socket.
    ASSERT_TRUE(waitFor([&]() { return guestKeyframeEpoch != 0; }, 5000));

    // A local change to a host-owned control on the guest side is reverted and
    // reported, which exercises the full socket-to-control path.
    int blockedDeck = -1;
    QString blockedItem;
    QObject::connect(&guest,
            &CollabConnection::localInputBlocked,
            [&](int deck, const QString& item) {
                blockedDeck = deck;
                blockedItem = item;
            });
    hostVolume.set(0.9);
    EXPECT_EQ(blockedDeck, 1);
    EXPECT_EQ(blockedItem, QStringLiteral("volume"));
    EXPECT_DOUBLE_EQ(hostVolume.get(), 0.3);
}

TEST_F(CollabConnectionTest, ReconnectsAfterPeerLoss) {
    ControlObject volume(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")));
    ControlObject crossfader(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
    volume.set(0.25);

    const CollabSessionToken token = collabTokenFromCode(QStringLiteral("TEST42"));
    const QList<ConfigKey> keys = testControlKeys();

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
    host.setControlKeys(keys);
    guest.setControlKeys(keys);
    host.setSessionTimeouts(fastConnectionTimeouts());
    guest.setSessionTimeouts(fastConnectionTimeouts());

    uint32_t guestKeyframeEpoch = 0;
    QObject::connect(&guest,
            &CollabConnection::controlKeyframeApplied,
            [&guestKeyframeEpoch](uint32_t epoch) {
                guestKeyframeEpoch = std::max(guestKeyframeEpoch, epoch);
            });

    ASSERT_TRUE(host.host(0, token, QString()));
    ASSERT_TRUE(guest.join(
            QStringLiteral("127.0.0.1"), host.localPort(), 0, token, QString()));
    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));
    ASSERT_TRUE(waitFor([&]() { return guestKeyframeEpoch != 0; }, 5000));
    const uint32_t epochBefore = guestKeyframeEpoch;

    ASSERT_TRUE(host.reconnect());
    EXPECT_TRUE(host.reconnecting());
    EXPECT_TRUE(host.remoteDecksStale());

    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));
    EXPECT_FALSE(host.reconnecting());
    EXPECT_FALSE(host.remoteDecksStale());

    // The host restarted its replicator, so its next keyframe carries a higher
    // epoch and the guest accepts it instead of treating it as stale.
    ASSERT_TRUE(waitFor(
            [&]() { return guestKeyframeEpoch > epochBefore; }, 5000));
}

TEST_F(CollabConnectionTest, GuestReconnectGetsAFreshSnapshot) {
    ControlObject volume(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")));
    ControlObject crossfader(ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")));
    volume.set(0.25);

    const CollabSessionToken token = collabTokenFromCode(QStringLiteral("TEST42"));
    const QList<ConfigKey> keys = testControlKeys();

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
    host.setControlKeys(keys);
    guest.setControlKeys(keys);
    host.setSessionTimeouts(fastConnectionTimeouts());
    guest.setSessionTimeouts(fastConnectionTimeouts());

    uint32_t guestKeyframeEpoch = 0;
    int guestKeyframes = 0;
    QObject::connect(&guest,
            &CollabConnection::controlKeyframeApplied,
            [&](uint32_t epoch) {
                guestKeyframeEpoch = std::max(guestKeyframeEpoch, epoch);
                ++guestKeyframes;
            });

    ASSERT_TRUE(host.host(0, token, QString()));
    ASSERT_TRUE(guest.join(
            QStringLiteral("127.0.0.1"), host.localPort(), 0, token, QString()));
    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));
    ASSERT_TRUE(waitFor([&]() { return guestKeyframes > 0; }, 5000));
    const int keyframesBefore = guestKeyframes;

    // The guest restarts its handshake while the host stays connected. The
    // host must push a fresh snapshot instead of waiting for the periodic one.
    ASSERT_TRUE(guest.reconnect());
    EXPECT_TRUE(guest.reconnecting());
    ASSERT_TRUE(waitFor(
            [&]() { return host.isConnected() && guest.isConnected(); }, 5000));
    EXPECT_FALSE(guest.reconnecting());
    EXPECT_TRUE(waitFor(
            [&]() { return guestKeyframes > keyframesBefore; }, 2000));
    EXPECT_GE(guestKeyframeEpoch, 1u);
}

TEST_F(CollabConnectionTest, InvalidPeerAddressIsReported) {
    CollabConnection connection;
    EXPECT_FALSE(connection.join(
            QStringLiteral("not an address"), 40410, 40411, makeCollabSessionToken(), QString()));
    EXPECT_FALSE(connection.isActive());
    EXPECT_TRUE(connection.errorString().contains(QStringLiteral("address"),
            Qt::CaseInsensitive));
}

} // namespace collab
} // namespace mixxx
