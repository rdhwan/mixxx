#include "collab/tracktransfer.h"

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
#include <vector>

#include "collab/trackcache.h"
#include "collab/trackhash.h"

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

QByteArray makeContent(int size, int seed) {
    QByteArray content(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) {
        content[i] = static_cast<char>((i * 31 + seed) & 0xff);
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

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

struct TestPeer {
    explicit TestPeer(const QString& root)
            : cache(root) {
        QString error;
        EXPECT_TRUE(cache.ensureRoot(&error)) << error.toStdString();
        EXPECT_TRUE(service.listen(0, &error)) << error.toStdString();
    }

    CollabTrackCache cache;
    CollabTransferService service{&cache};
};

} // namespace

TEST(CollabTransferServiceTest, TransfersAndInstallsAVerifiedFile) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(1024 * 1024 + 12345, 7);
    const QString source = writeFile(
            senderDir.filePath(QStringLiteral("library/track.flac")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    QString finishedPath;
    QString failure;
    bool served = false;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });
    QObject::connect(&sender.service,
            &CollabTransferService::serveFinished,
            [&](const CollabContentHash&) { served = true; });

    QString error;
    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("flac"),
            &error))
            << error.toStdString();
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty() || !failure.isEmpty(); }))
            << "transfer did not finish";

    EXPECT_TRUE(failure.isEmpty()) << failure.toStdString();
    ASSERT_FALSE(finishedPath.isEmpty());
    EXPECT_EQ(readFile(finishedPath), content);
    EXPECT_EQ(QFileInfo(finishedPath).absolutePath(),
            QFileInfo(receiver.cache.pathForHash(hash, QStringLiteral("flac")))
                    .absolutePath());
    EXPECT_FALSE(QFile::exists(receiver.cache.partialPathForHash(hash)));
    EXPECT_TRUE(waitFor([&]() { return served; }));
}

TEST(CollabTransferServiceTest, TransfersAnEmptyFile) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QString source =
            writeFile(senderDir.filePath(QStringLiteral("empty.mp3")), QByteArray());
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    QString finishedPath;
    QString failure;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("mp3")));
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty() || !failure.isEmpty(); }));

    EXPECT_TRUE(failure.isEmpty()) << failure.toStdString();
    ASSERT_FALSE(finishedPath.isEmpty());
    EXPECT_TRUE(QFileInfo(finishedPath).exists());
    EXPECT_EQ(QFileInfo(finishedPath).size(), 0);
}

TEST(CollabTransferServiceTest, ResumesFromAnExistingPartialFile) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(512 * 1024, 3);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.wav")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    constexpr qint64 kPrefixBytes = 200000;
    const QString partialPath = receiver.cache.partialPathForHash(hash);
    writeFile(partialPath, content.left(kPrefixBytes));

    QString finishedPath;
    QString failure;
    qint64 firstReported = -1;
    qint64 lastReported = -1;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchProgress,
            [&](const CollabContentHash&, qint64 received, qint64) {
                if (firstReported < 0) {
                    firstReported = received;
                }
                lastReported = received;
            });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("wav")));
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty() || !failure.isEmpty(); }));

    EXPECT_TRUE(failure.isEmpty()) << failure.toStdString();
    ASSERT_FALSE(finishedPath.isEmpty());
    EXPECT_EQ(firstReported, kPrefixBytes);
    EXPECT_EQ(lastReported, content.size());
    EXPECT_EQ(readFile(finishedPath), content);
}

TEST(CollabTransferServiceTest, RejectsACorruptPartialFile) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(64 * 1024, 11);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.flac")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    const QString partialPath = receiver.cache.partialPathForHash(hash);
    QByteArray corrupt = content;
    corrupt[0] = static_cast<char>(corrupt[0] ^ 0xff);
    writeFile(partialPath, corrupt);

    QString finishedPath;
    QString failure;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("flac")));
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty() || !failure.isEmpty(); }));

    EXPECT_TRUE(finishedPath.isEmpty());
    EXPECT_FALSE(failure.isEmpty());
    EXPECT_FALSE(QFile::exists(partialPath));
    EXPECT_FALSE(receiver.cache.find(hash).has_value());
}

TEST(CollabTransferServiceTest, FailsWhenThePeerDoesNotHaveTheTrack) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(4096, 5);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.mp3")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));

    QString failure;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("mp3")));
    ASSERT_TRUE(waitFor([&]() { return !failure.isEmpty(); }));

    EXPECT_FALSE(receiver.cache.find(hash).has_value());
}

TEST(CollabTransferServiceTest, FailsWhenThePartialIsLargerThanTheSource) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(8192, 9);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.mp3")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    writeFile(receiver.cache.partialPathForHash(hash), makeContent(16384, 1));

    QString failure;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("mp3")));
    ASSERT_TRUE(waitFor([&]() { return !failure.isEmpty(); }));
    EXPECT_FALSE(receiver.cache.find(hash).has_value());
}

TEST(CollabTransferServiceTest, RejectsAConnectionFromAnotherAddress) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(4096, 13);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.mp3")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);
    sender.service.setAllowedPeer(QHostAddress(QStringLiteral("192.0.2.1")));

    QString finishedPath;
    QString failure;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) { failure = error; });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("mp3")));
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty() || !failure.isEmpty(); }));

    EXPECT_TRUE(finishedPath.isEmpty());
    EXPECT_FALSE(failure.isEmpty());
}

TEST(CollabTransferServiceTest, CancelledFetchCanBeResumed) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(2 * 1024 * 1024, 17);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("big.flac")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    QString finishedPath;
    std::vector<QString> failures;
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFinished,
            [&](const CollabContentHash&, const QString& path) { finishedPath = path; });
    QObject::connect(&receiver.service,
            &CollabTransferService::fetchFailed,
            [&](const CollabContentHash&, const QString& error) {
                failures.push_back(error);
            });

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("flac")));
    receiver.service.cancelFetch(hash);
    ASSERT_TRUE(waitFor([&]() { return failures.size() == 1; }));
    EXPECT_FALSE(receiver.service.isFetching(hash));

    ASSERT_TRUE(receiver.service.fetch(QHostAddress::LocalHost,
            sender.service.localPort(),
            hash,
            QStringLiteral("flac")));
    ASSERT_TRUE(waitFor([&]() { return !finishedPath.isEmpty(); }));

    EXPECT_EQ(readFile(finishedPath), content);
}

TEST(CollabTransferServiceTest, RejectsDuplicateAndInvalidFetches) {
    QTemporaryDir senderDir;
    QTemporaryDir receiverDir;
    ASSERT_TRUE(senderDir.isValid());
    ASSERT_TRUE(receiverDir.isValid());
    TestPeer sender(senderDir.filePath(QStringLiteral("cache")));
    TestPeer receiver(receiverDir.filePath(QStringLiteral("cache")));

    const QByteArray content = makeContent(1024 * 1024, 21);
    const QString source = writeFile(senderDir.filePath(QStringLiteral("track.flac")), content);
    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(source, &hash));
    sender.service.serveFile(hash, source);

    QString error;
    EXPECT_FALSE(receiver.service.fetch(
            QHostAddress::LocalHost, sender.service.localPort(), CollabContentHash{}, QStringLiteral("flac"), &error));
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    EXPECT_FALSE(receiver.service.fetch(
            QHostAddress::LocalHost, 0, hash, QStringLiteral("flac"), &error));
    EXPECT_FALSE(error.isEmpty());

    ASSERT_TRUE(receiver.service.fetch(
            QHostAddress::LocalHost, sender.service.localPort(), hash, QStringLiteral("flac"), &error));
    EXPECT_TRUE(receiver.service.isFetching(hash));
    error.clear();
    EXPECT_FALSE(receiver.service.fetch(
            QHostAddress::LocalHost, sender.service.localPort(), hash, QStringLiteral("flac"), &error));
    EXPECT_FALSE(error.isEmpty());

    receiver.service.cancelFetch(hash);
    EXPECT_TRUE(waitFor([&]() { return !receiver.service.isFetching(hash); }));
}

} // namespace collab
} // namespace mixxx
