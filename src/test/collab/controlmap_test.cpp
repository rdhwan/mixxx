#include "collab/controlmap.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QList>
#include <QPair>
#include <QString>
#include <optional>

#include "preferences/configobject.h"

namespace mixxx {
namespace collab {

namespace {

void writeString(QDataStream* pStream, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    *pStream << static_cast<quint16>(utf8.size());
    pStream->writeRawData(utf8.constData(), utf8.size());
}

QByteArray makeMapPayload(quint8 version,
        quint16 count,
        const QList<QPair<quint16, ConfigKey>>& entries) {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << version << count;
    for (const auto& entry : entries) {
        stream << entry.first;
        writeString(&stream, entry.second.group);
        writeString(&stream, entry.second.item);
    }
    return data;
}

} // namespace

TEST(CollabControlMapTest, FromKeysAssignsStableSortedIds) {
    const QList<ConfigKey> keys = {
            ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play")),
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("play")),
    };

    const CollabControlMap map = CollabControlMap::fromKeys(keys);

    ASSERT_EQ(map.size(), 4);
    ASSERT_EQ(map.keyFor(1), std::optional<ConfigKey>(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("play"))));
    ASSERT_EQ(map.keyFor(2), std::optional<ConfigKey>(ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))));
    ASSERT_EQ(map.keyFor(3), std::optional<ConfigKey>(ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("play"))));
    ASSERT_EQ(map.keyFor(4), std::optional<ConfigKey>(ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("volume"))));
}

TEST(CollabControlMapTest, DuplicateKeysAreIgnored) {
    QList<ConfigKey> keys = {
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")),
    };

    const CollabControlMap map = CollabControlMap::fromKeys(keys);

    EXPECT_EQ(map.size(), 1);
    EXPECT_EQ(map.idFor(ConfigKey(QStringLiteral("[Channel1]"),
                      QStringLiteral("volume"))),
            std::optional<CollabControlId>(1));
}

TEST(CollabControlMapTest, AddRejectsInvalidAndDuplicateKeys) {
    CollabControlMap map;

    EXPECT_FALSE(map.add(ConfigKey()));
    EXPECT_TRUE(map.add(ConfigKey(QStringLiteral("[Channel1]"),
            QStringLiteral("volume"))));
    EXPECT_FALSE(map.add(ConfigKey(QStringLiteral("[Channel1]"),
            QStringLiteral("volume"))));
    EXPECT_EQ(map.size(), 1);
}

TEST(CollabControlMapTest, UnknownIdAndKeyReturnNullopt) {
    const CollabControlMap map = CollabControlMap::fromKeys(
            {ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))});

    EXPECT_EQ(map.keyFor(kInvalidCollabControlId), std::nullopt);
    EXPECT_EQ(map.keyFor(99), std::nullopt);
    EXPECT_EQ(map.idFor(ConfigKey(QStringLiteral("[Channel1]"),
                      QStringLiteral("missing"))),
            std::nullopt);
}

TEST(CollabControlMapTest, ByteArrayRoundTrip) {
    const QList<ConfigKey> keys = {
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume")),
            ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader")),
            ConfigKey(QStringLiteral("[Channel2]"), QStringLiteral("filterLow")),
    };
    const CollabControlMap original = CollabControlMap::fromKeys(keys);

    QString error;
    const auto parsed = CollabControlMap::fromByteArray(
            original.toByteArray(), &error);

    ASSERT_TRUE(parsed.has_value()) << error.toStdString();
    EXPECT_EQ(parsed->entries().size(), original.entries().size());
    for (const auto& entry : original.entries()) {
        EXPECT_EQ(parsed->keyFor(entry.id), std::optional<ConfigKey>(entry.key));
        EXPECT_EQ(parsed->idFor(entry.key),
                std::optional<CollabControlId>(entry.id));
    }
}

TEST(CollabControlMapTest, EmptyMapRoundTrip) {
    const CollabControlMap original;

    const auto parsed = CollabControlMap::fromByteArray(original.toByteArray());

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->isEmpty());
}

TEST(CollabControlMapTest, RejectsUnsupportedVersion) {
    const QByteArray payload = makeMapPayload(
            2,
            1,
            {{1, ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))}});

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

TEST(CollabControlMapTest, RejectsTruncatedPayload) {
    QByteArray payload = makeMapPayload(
            1,
            1,
            {{1, ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))}});
    payload.chop(3);

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

TEST(CollabControlMapTest, RejectsInvalidControlId) {
    const QByteArray payload = makeMapPayload(
            1,
            1,
            {{0, ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))}});

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

TEST(CollabControlMapTest, RejectsDuplicateControlId) {
    const QByteArray payload = makeMapPayload(
            1,
            2,
            {{1, ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))},
                    {1,
                            ConfigKey(QStringLiteral("[Channel2]"),
                                    QStringLiteral("volume"))}});

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

TEST(CollabControlMapTest, RejectsDuplicateControlKey) {
    const QByteArray payload = makeMapPayload(
            1,
            2,
            {{1, ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("volume"))},
                    {2,
                            ConfigKey(QStringLiteral("[Channel1]"),
                                    QStringLiteral("volume"))}});

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

TEST(CollabControlMapTest, RejectsTrailingData) {
    QByteArray payload = makeMapPayload(
            1,
            0,
            {});
    payload.append('\0');

    EXPECT_FALSE(CollabControlMap::fromByteArray(payload).has_value());
}

} // namespace collab
} // namespace mixxx
