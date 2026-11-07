#include "collab/trackhash.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

namespace mixxx {
namespace collab {

namespace {

QString writeFile(const QTemporaryDir& dir,
        const QString& name,
        const QByteArray& content) {
    const QString path = dir.filePath(name);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(content), content.size());
    file.close();
    return path;
}

constexpr char kAbcSha256[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr char kEmptySha256[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

} // namespace

TEST(CollabTrackHashTest, HashesKnownContent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "abc.bin", QByteArrayLiteral("abc"));

    CollabContentHash hash;
    QString error;
    ASSERT_TRUE(collabHashFile(path, &hash, &error)) << error.toStdString();
    EXPECT_TRUE(hash.isValid());
    EXPECT_EQ(hash.toHex(), QString::fromLatin1(kAbcSha256));
}

TEST(CollabTrackHashTest, HashesEmptyFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "empty.bin", QByteArray());

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));
    EXPECT_EQ(hash.toHex(), QString::fromLatin1(kEmptySha256));
}

TEST(CollabTrackHashTest, MissingFileFails) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabContentHash hash;
    QString error;
    EXPECT_FALSE(collabHashFile(dir.filePath(QStringLiteral("missing")), &hash, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(CollabTrackHashTest, HexRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "abc.bin", QByteArrayLiteral("abc"));

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));
    const auto decoded = CollabContentHash::fromHex(hash.toHex());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, hash);

    EXPECT_FALSE(CollabContentHash::fromHex(QStringLiteral("00")).has_value());
    EXPECT_FALSE(CollabContentHash::fromHex(QStringLiteral("zz")).has_value());
}

TEST(CollabTrackHashIndexTest, InsertAndLookup) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "track.mp3", QByteArrayLiteral("audio"));

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));

    CollabTrackHashIndex index;
    ASSERT_TRUE(index.insert(path, hash));
    EXPECT_EQ(index.size(), 1);
    const auto cached = index.lookup(path);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(*cached, hash);
}

TEST(CollabTrackHashIndexTest, SizeChangeInvalidates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "track.mp3", QByteArrayLiteral("audio"));

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));
    CollabTrackHashIndex index;
    ASSERT_TRUE(index.insert(path, hash));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::Append));
    ASSERT_EQ(file.write("more"), 4);
    file.close();

    EXPECT_FALSE(index.lookup(path).has_value());
}

TEST(CollabTrackHashIndexTest, ModificationTimeChangeInvalidates) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "track.mp3", QByteArrayLiteral("audio"));

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));
    CollabTrackHashIndex index;
    ASSERT_TRUE(index.insert(path, hash));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.setFileTime(QDateTime::currentDateTime().addSecs(10),
            QFileDevice::FileModificationTime));
    file.close();

    EXPECT_FALSE(index.lookup(path).has_value());
}

TEST(CollabTrackHashIndexTest, SaveAndLoadRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "track.flac", QByteArrayLiteral("audio"));
    const QString indexPath = dir.filePath(QStringLiteral("index/hashes.json"));

    CollabContentHash hash;
    ASSERT_TRUE(collabHashFile(path, &hash));

    CollabTrackHashIndex index;
    ASSERT_TRUE(index.insert(path, hash));
    QString error;
    ASSERT_TRUE(index.save(indexPath, &error)) << error.toStdString();
    EXPECT_TRUE(QFile::exists(indexPath));

    CollabTrackHashIndex loaded;
    ASSERT_TRUE(loaded.load(indexPath, &error)) << error.toStdString();
    EXPECT_EQ(loaded.size(), 1);
    const auto cached = loaded.lookup(path);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(*cached, hash);
}

TEST(CollabTrackHashIndexTest, MissingIndexLoadsEmpty) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    CollabTrackHashIndex index;
    QString error;
    EXPECT_TRUE(index.load(dir.filePath(QStringLiteral("missing.json")), &error));
    EXPECT_TRUE(error.isEmpty());
    EXPECT_EQ(index.size(), 0);
}

TEST(CollabTrackHashIndexTest, MalformedIndexFails) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString indexPath = writeFile(
            dir, "broken.json", QByteArrayLiteral("not json"));

    CollabTrackHashIndex index;
    QString error;
    EXPECT_FALSE(index.load(indexPath, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(CollabTrackHashIndexTest, RemoveAndClear) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString first = writeFile(dir, "one.mp3", QByteArrayLiteral("one"));
    const QString second = writeFile(dir, "two.mp3", QByteArrayLiteral("two"));

    CollabTrackHashIndex index;
    CollabContentHash firstHash;
    CollabContentHash secondHash;
    ASSERT_TRUE(collabHashFile(first, &firstHash));
    ASSERT_TRUE(collabHashFile(second, &secondHash));
    ASSERT_TRUE(index.insert(first, firstHash));
    ASSERT_TRUE(index.insert(second, secondHash));

    index.remove(first);
    EXPECT_FALSE(index.lookup(first).has_value());
    EXPECT_TRUE(index.lookup(second).has_value());

    index.clear();
    EXPECT_EQ(index.size(), 0);
    EXPECT_FALSE(index.lookup(second).has_value());
}

TEST(CollabTrackHashIndexTest, RejectsInvalidInput) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir, "track.mp3", QByteArrayLiteral("audio"));

    CollabTrackHashIndex index;
    EXPECT_FALSE(index.insert(path, CollabContentHash{}));
    EXPECT_FALSE(index.insert(
            dir.filePath(QStringLiteral("missing")), CollabContentHash{QByteArray(32, 0)}));
}

} // namespace collab
} // namespace mixxx
