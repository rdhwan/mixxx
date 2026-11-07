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

TEST(CollabControlMapTest, DefaultKeysUseTheResultingRate) {
    const QList<ConfigKey> keys = collabDefaultControlKeys();

    EXPECT_TRUE(keys.contains(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("rate_ratio"))));
    EXPECT_FALSE(keys.contains(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("rate"))));
    EXPECT_TRUE(keys.contains(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("reverse"))));
    EXPECT_TRUE(keys.contains(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("cue_default"))));
    EXPECT_TRUE(keys.contains(
            ConfigKey(QStringLiteral("[Channel1]"), QStringLiteral("orientation"))));
}

TEST(CollabControlMapTest, DefaultKeysIncludeTheEffectControls) {
    const QList<ConfigKey> keys = collabDefaultControlKeys();

    // Standard FX unit: chain state and deck routing.
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit1]"),
            QStringLiteral("enabled"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit1]"),
            QStringLiteral("super1"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit4]"),
            QStringLiteral("group_[Channel3]_enable"))));
    // Standard FX slot: loaded effect, meta, parameters.
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"),
            QStringLiteral("loaded_effect"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"),
            QStringLiteral("meta"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"),
            QStringLiteral("parameter16"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[EffectRack1_EffectUnit2_Effect3]"),
            QStringLiteral("button_parameter16"))));
    // Per-deck quick effect chain.
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[QuickEffectRack1_[Channel4]]"),
            QStringLiteral("super1"))));
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[QuickEffectRack1_[Channel4]_Effect1]"),
            QStringLiteral("loaded_effect"))));

    // The quick effect groups embed the deck channel name, so they must be
    // derived for every session deck, not only the first one.
    EXPECT_TRUE(keys.contains(ConfigKey(
            QStringLiteral("[QuickEffectRack1_[Channel1]_Effect1]"),
            QStringLiteral("loaded_effect"))));
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
