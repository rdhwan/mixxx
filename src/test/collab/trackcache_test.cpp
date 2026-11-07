#include "collab/trackcache.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace mixxx {
namespace collab {

namespace {

QString writeBytes(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(content), content.size());
    file.close();
    return path;
}

CollabContentHash hashBytes(const QByteArray& content) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    const QString path = writeBytes(dir.filePath(QStringLiteral("source")), content);
    CollabContentHash hash;
    EXPECT_TRUE(collabHashFile(path, &hash));
    return hash;
}

} // namespace

TEST(CollabTrackCacheTest, PathForHashIsContentAddressed) {
    const CollabContentHash first = hashBytes(QByteArrayLiteral("first"));
    const CollabContentHash second = hashBytes(QByteArrayLiteral("second"));
    ASSERT_TRUE(first.isValid());
    ASSERT_TRUE(second.isValid());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    EXPECT_EQ(cache.pathForHash(first, QStringLiteral("mp3")),
            cache.pathForHash(first, QStringLiteral(".MP3")));
    EXPECT_NE(cache.pathForHash(first, QStringLiteral("mp3")),
            cache.pathForHash(second, QStringLiteral("mp3")));
    EXPECT_TRUE(cache.pathForHash(first, QStringLiteral("mp3"))
                    .endsWith(first.toHex() + QStringLiteral(".mp3")));
}

TEST(CollabTrackCacheTest, InvalidSuffixIsDropped) {
    const CollabContentHash hash = hashBytes(QByteArrayLiteral("audio"));
    ASSERT_TRUE(hash.isValid());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    const QString path = cache.pathForHash(hash, QStringLiteral("../../evil"));
    EXPECT_EQ(QFileInfo(path).fileName(), hash.toHex());
}

TEST(CollabTrackCacheTest, EnsureRootCreatesDirectories) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));

    QString error;
    ASSERT_TRUE(cache.ensureRoot(&error)) << error.toStdString();
    EXPECT_TRUE(QDir(cache.root()).exists());
    EXPECT_TRUE(QDir(cache.root() + QStringLiteral("/partial")).exists());
}

TEST(CollabTrackCacheTest, InstallVerifiesAndMoves) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const QByteArray content = QByteArrayLiteral("track bytes");
    const CollabContentHash hash = hashBytes(content);
    const QString partial = writeBytes(cache.partialPathForHash(hash), content);

    QString error;
    ASSERT_TRUE(cache.install(hash, partial, QStringLiteral("mp3"), &error))
            << error.toStdString();
    EXPECT_FALSE(QFile::exists(partial));

    const auto installed = cache.find(hash);
    ASSERT_TRUE(installed.has_value());
    QFile file(*installed);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), content);
}

TEST(CollabTrackCacheTest, InstallRejectsCorruptPartial) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const CollabContentHash expected = hashBytes(QByteArrayLiteral("expected"));
    const QString partial =
            writeBytes(cache.partialPathForHash(expected), QByteArrayLiteral("corrupt"));

    QString error;
    EXPECT_FALSE(cache.install(expected, partial, QStringLiteral("mp3"), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(partial));
    EXPECT_FALSE(cache.find(expected).has_value());
}

TEST(CollabTrackCacheTest, InstallIsIdempotent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const QByteArray content = QByteArrayLiteral("same bytes");
    const CollabContentHash hash = hashBytes(content);
    const QString firstPartial = writeBytes(cache.partialPathForHash(hash), content);
    ASSERT_TRUE(cache.install(hash, firstPartial, QStringLiteral("flac")));
    const auto firstPath = cache.find(hash);
    ASSERT_TRUE(firstPath.has_value());

    const QString secondPartial = writeBytes(cache.partialPathForHash(hash), content);
    ASSERT_TRUE(cache.install(hash, secondPartial, QStringLiteral("flac")));
    EXPECT_FALSE(QFile::exists(secondPartial));
    EXPECT_EQ(cache.find(hash), firstPath);
}

TEST(CollabTrackCacheTest, VerifyDetectsCorruption) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const QByteArray content = QByteArrayLiteral("good bytes");
    const CollabContentHash hash = hashBytes(content);
    const QString partial = writeBytes(cache.partialPathForHash(hash), content);
    ASSERT_TRUE(cache.install(hash, partial, QStringLiteral("wav")));
    const auto installed = cache.find(hash);
    ASSERT_TRUE(installed.has_value());

    QFile file(*installed);
    ASSERT_TRUE(file.open(QIODevice::Append));
    ASSERT_EQ(file.write("x"), 1);
    file.close();

    QString error;
    EXPECT_FALSE(cache.verify(hash, *installed, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(CollabTrackCacheTest, FindReturnsNulloptForMissing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const CollabContentHash hash = hashBytes(QByteArrayLiteral("absent"));
    EXPECT_FALSE(cache.find(hash).has_value());
}

TEST(CollabTrackCacheTest, RejectsInvalidHash) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    const CollabContentHash invalid;
    EXPECT_TRUE(cache.pathForHash(invalid, QStringLiteral("mp3")).isEmpty());
    EXPECT_TRUE(cache.partialPathForHash(invalid).isEmpty());
    EXPECT_FALSE(cache.find(invalid).has_value());
    EXPECT_FALSE(cache.install(invalid, cache.root(), QStringLiteral("mp3")));
    EXPECT_FALSE(cache.verify(invalid, cache.root()));
}

TEST(CollabTrackCacheTest, SpaceChecks) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackCache cache(dir.filePath(QStringLiteral("cache")));
    ASSERT_TRUE(cache.ensureRoot());

    EXPECT_TRUE(cache.hasSpaceFor(1));
    EXPECT_FALSE(cache.hasSpaceFor(-1));
}

} // namespace collab
} // namespace mixxx
