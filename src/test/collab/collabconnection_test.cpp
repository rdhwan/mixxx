#include "collab/collabconnection.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <functional>

#include "collab/collabprotocol.h"

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

} // namespace

TEST(CollabConnectionTest, HostAndGuestSynchroniseTheirClocks) {
    const CollabSessionToken token = collabTokenFromCode(QStringLiteral("TEST42"));

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);

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

TEST(CollabConnectionTest, LeavingEndsTheOtherSide) {
    const CollabSessionToken token = makeCollabSessionToken();

    CollabConnection host;
    CollabConnection guest;
    host.setClockIntervalMs(20);
    guest.setClockIntervalMs(20);
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

TEST(CollabConnectionTest, InvalidPeerAddressIsReported) {
    CollabConnection connection;
    EXPECT_FALSE(connection.join(
            QStringLiteral("not an address"), 40410, 40411,
            makeCollabSessionToken(), QString()));
    EXPECT_FALSE(connection.isActive());
    EXPECT_TRUE(connection.errorString().contains(QStringLiteral("address"),
            Qt::CaseInsensitive));
}

} // namespace collab
} // namespace mixxx
