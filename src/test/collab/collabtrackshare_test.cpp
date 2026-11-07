#include "collab/collabtrackshare.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QThread>
#include <functional>

#include "collab/collabsession.h"
#include "collab/trackcache.h"
#include "collab/trackhash.h"
#include "collab/tracktransfer.h"

namespace mixxx {
namespace collab {

namespace {

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

CollabSessionTimeouts fastTimeouts() {
    CollabSessionTimeouts timeouts;
    timeouts.handshakeMs = 2000;
    timeouts.handshakeRetryMs = 50;
    timeouts.keepaliveMs = 150;
    timeouts.peerTimeoutMs = 600;
    return timeouts;
}

QByteArray makeContent(int size, int seed) {
    QByteArray content(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) {
        content[i] = static_cast<char>((i * 17 + seed) & 0xff);
    }
    return content;
}

QString writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(content), content.size());
    file.close();
    return path;
}

struct Peer {
    explicit Peer(const QString& root)
            : cache(root),
              transfer(&cache),
              share(&session, &transfer, &index, &cache) {
        QString error;
        EXPECT_TRUE(cache.ensureRoot(&error)) << error.toStdString();
        EXPECT_TRUE(transfer.listen(0, &error)) << error.toStdString();
        transfer.setAllowedPeer(QHostAddress::LocalHost);
        session.setTimeouts(fastTimeouts());
    }

    CollabTrackCache cache;
    CollabTrackHashIndex index;
    CollabTransferService transfer;
    CollabSession session;
    CollabTrackShare share;
};

struct Link {
    QTemporaryDir hostDir;
    QTemporaryDir guestDir;
    Peer host;
    Peer guest;

    Link()
            : host(hostDir.filePath(QStringLiteral("cache"))),
              guest(guestDir.filePath(QStringLiteral("cache"))) {
        QObject::connect(&host.session,
                &CollabSession::packetReceived,
                &host.share,
                [this](const CollabPacket& packet) {
                    host.share.handlePacket(packet);
                });
        QObject::connect(&guest.session,
                &CollabSession::packetReceived,
                &guest.share,
                [this](const CollabPacket& packet) {
                    guest.share.handlePacket(packet);
                });
    }

    bool connectPeers() {
        const CollabSessionToken token = makeCollabSessionToken();
        if (!host.session.host(0, token)) {
            return false;
        }
        if (!guest.session.join(
                    QHostAddress::LocalHost, host.session.localPort(), 0, token)) {
            return false;
        }
        return waitFor([this]() {
            return host.session.state() == CollabSession::State::Connected &&
                    guest.session.state() == CollabSession::State::Connected;
        });
    }
};

} // namespace

TEST(CollabTrackShareTest, TransfersThenReportsReady) {
    Link link;
    ASSERT_TRUE(link.hostDir.isValid());
    ASSERT_TRUE(link.guestDir.isValid());
    ASSERT_TRUE(link.connectPeers());

    const QByteArray content = makeContent(256 * 1024, 3);
    const QString source =
            writeFile(link.hostDir.filePath(QStringLiteral("host/track.flac")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));

    int requestedDeck = 0;
    QString requestedPath;
    bool guestCompleted = false;
    bool hostCompleted = false;
    QString hostFailure;
    QObject::connect(&link.guest.share,
            &CollabTrackShare::loadRequested,
            [&](int deck, const CollabContentHash& requestedHash, const QString& path) {
                requestedDeck = deck;
                requestedPath = path;
                link.guest.share.reportLoadResult(deck, requestedHash, true);
            });
    QObject::connect(&link.guest.share,
            &CollabTrackShare::loadCompleted,
            [&](int, const CollabContentHash&) { guestCompleted = true; });
    QObject::connect(&link.host.share,
            &CollabTrackShare::loadCompleted,
            [&](int, const CollabContentHash&) { hostCompleted = true; });
    QObject::connect(&link.host.share,
            &CollabTrackShare::loadFailed,
            [&](int, const CollabContentHash&, const QString& reason) {
                hostFailure = reason;
            });

    QString error;
    ASSERT_TRUE(link.host.share.requestLoad(1, source, &error))
            << error.toStdString();
    EXPECT_TRUE(link.host.share.hasPendingLoad(1));

    ASSERT_TRUE(waitFor([&]() { return hostCompleted || !hostFailure.isEmpty(); }))
            << "load did not complete";
    EXPECT_TRUE(hostFailure.isEmpty()) << hostFailure.toStdString();
    EXPECT_TRUE(hostCompleted);
    EXPECT_TRUE(guestCompleted);
    EXPECT_EQ(requestedDeck, 1);
    ASSERT_FALSE(requestedPath.isEmpty());
    EXPECT_EQ(QFileInfo(requestedPath).size(), content.size());
    EXPECT_FALSE(link.host.share.hasPendingLoad(1));
    EXPECT_FALSE(link.guest.share.hasPendingLoad(1));

    // The received copy is installed under its content hash.
    const auto installed = link.guest.cache.find(hash);
    ASSERT_TRUE(installed.has_value());
    EXPECT_EQ(*installed, requestedPath);
}

TEST(CollabTrackShareTest, SkipsTransferWhenThePeerAlreadyHasTheFile) {
    Link link;
    ASSERT_TRUE(link.connectPeers());

    const QByteArray content = makeContent(32 * 1024, 7);
    const QString source =
            writeFile(link.hostDir.filePath(QStringLiteral("host/track.mp3")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));

    // Install the same content in the guest cache first.
    const QString partial = writeFile(
            link.guest.cache.partialPathForHash(hash), content);
    ASSERT_TRUE(link.guest.cache.install(hash, partial, QStringLiteral("mp3")));

    int requestedDeck = 0;
    bool hostCompleted = false;
    bool transferStarted = false;
    QObject::connect(&link.guest.share,
            &CollabTrackShare::loadRequested,
            [&](int deck, const CollabContentHash& requestedHash, const QString&) {
                requestedDeck = deck;
                link.guest.share.reportLoadResult(deck, requestedHash, true);
            });
    QObject::connect(&link.guest.share,
            &CollabTrackShare::transferProgress,
            [&](const CollabContentHash&, qint64, qint64) { transferStarted = true; });
    QObject::connect(&link.host.share,
            &CollabTrackShare::loadCompleted,
            [&](int, const CollabContentHash&) { hostCompleted = true; });

    ASSERT_TRUE(link.host.share.requestLoad(2, source));
    ASSERT_TRUE(waitFor([&]() { return hostCompleted; }));
    EXPECT_EQ(requestedDeck, 2);
    EXPECT_FALSE(transferStarted);
}

TEST(CollabTrackShareTest, ReportsATransferFailure) {
    Link link;
    ASSERT_TRUE(link.connectPeers());

    const QByteArray content = makeContent(16 * 1024, 11);
    const QString source =
            writeFile(link.hostDir.filePath(QStringLiteral("host/track.wav")), content);

    bool guestFailed = false;
    bool hostFailed = false;
    QString hostReason;
    QObject::connect(&link.guest.share,
            &CollabTrackShare::loadFailed,
            [&](int, const CollabContentHash&, const QString&) { guestFailed = true; });
    QObject::connect(&link.host.share,
            &CollabTrackShare::loadFailed,
            [&](int, const CollabContentHash&, const QString& reason) {
                hostFailed = true;
                hostReason = reason;
            });

    ASSERT_TRUE(link.host.share.requestLoad(1, source));
    // The sender no longer has the file when the peer asks for it.
    ASSERT_TRUE(QFile::remove(source));

    ASSERT_TRUE(waitFor([&]() { return hostFailed; }));
    EXPECT_TRUE(guestFailed);
    EXPECT_FALSE(hostReason.isEmpty());
    EXPECT_FALSE(link.host.share.hasPendingLoad(1));
    EXPECT_FALSE(link.guest.share.hasPendingLoad(1));
}

TEST(CollabTrackShareTest, ReportsALocalLoadFailure) {
    Link link;
    ASSERT_TRUE(link.connectPeers());

    const QByteArray content = makeContent(8 * 1024, 13);
    const QString source =
            writeFile(link.hostDir.filePath(QStringLiteral("host/track.flac")), content);

    bool hostFailed = false;
    QString hostReason;
    QObject::connect(&link.guest.share,
            &CollabTrackShare::loadRequested,
            [&](int deck, const CollabContentHash& requestedHash, const QString&) {
                link.guest.share.reportLoadResult(
                        deck, requestedHash, false, QStringLiteral("unsupported codec"));
            });
    QObject::connect(&link.host.share,
            &CollabTrackShare::loadFailed,
            [&](int, const CollabContentHash&, const QString& reason) {
                hostFailed = true;
                hostReason = reason;
            });

    ASSERT_TRUE(link.host.share.requestLoad(1, source));
    ASSERT_TRUE(waitFor([&]() { return hostFailed; }));
    EXPECT_EQ(hostReason, QStringLiteral("unsupported codec"));
}

TEST(CollabTrackShareTest, RejectsInvalidRequests) {
    Link link;

    QString error;
    EXPECT_FALSE(link.host.share.requestLoad(0, QStringLiteral("/nonexistent"), &error));
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_FALSE(link.host.share.requestLoad(1, QStringLiteral("/nonexistent"), &error));
    EXPECT_FALSE(error.isEmpty());

    // Not connected yet, so even an existing file is rejected.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source =
            writeFile(dir.filePath(QStringLiteral("track.mp3")), makeContent(1024, 1));
    error.clear();
    EXPECT_FALSE(link.host.share.requestLoad(1, source, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(CollabTrackShareTest, IgnoresUnrelatedPackets) {
    Link link;
    CollabPacket packet;
    packet.header.type = CollabMessageType::Keepalive;
    EXPECT_FALSE(link.host.share.handlePacket(packet));
}

} // namespace collab
} // namespace mixxx
