#include "collab/controlmap.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QList>
#include <QSet>
#include <QString>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include "collab/collabtypes.h"
#include "collab/deckmap.h"
#include "effects/defs.h"

namespace mixxx {
namespace collab {

namespace {

constexpr quint8 kControlMapVersion = 1;

/// EffectSlot creates this many knob and button parameter slots up front, so
/// the controls exist even while no effect is loaded.
constexpr int kDefaultEffectParameterSlots = 16;

/// The slot groups derive from the chain group by replacing the trailing ']'
/// with `_Effect<N>]`, matching EffectChain::addEffectSlot().
QString effectSlotGroup(const QString& chainGroup, int effectSlot) {
    QString group = chainGroup;
    group.chop(1);
    return group + QStringLiteral("_Effect%1]").arg(effectSlot);
}

QStringList effectSlotItems() {
    QStringList items = {
            QStringLiteral("enabled"),
            QStringLiteral("loaded_effect"),
            QStringLiteral("clear"),
            QStringLiteral("meta"),
    };
    for (int parameter = 1; parameter <= kDefaultEffectParameterSlots; ++parameter) {
        items.append(QStringLiteral("parameter%1").arg(parameter));
        items.append(QStringLiteral("button_parameter%1").arg(parameter));
    }
    return items;
}

void appendEffectChainKeys(QList<ConfigKey>* pKeys,
        const QString& chainGroup,
        const QStringList& routingDeckGroups) {
    const QStringList chainItems = {
            QStringLiteral("enabled"),
            QStringLiteral("mix"),
            QStringLiteral("super1"),
            QStringLiteral("mix_mode"),
    };
    for (const QString& item : chainItems) {
        pKeys->append(ConfigKey(chainGroup, item));
    }
    for (const QString& deckGroup : routingDeckGroups) {
        pKeys->append(ConfigKey(chainGroup,
                QStringLiteral("group_%1_enable").arg(deckGroup)));
    }
    const QStringList slotItems = effectSlotItems();
    for (int effectSlot = 1; effectSlot <= kNumEffectsPerUnit; ++effectSlot) {
        const QString slotGroup = effectSlotGroup(chainGroup, effectSlot);
        for (const QString& item : slotItems) {
            pKeys->append(ConfigKey(slotGroup, item));
        }
    }
}

bool writeString(QDataStream* pStream, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    if (utf8.isEmpty() || utf8.size() > std::numeric_limits<quint16>::max()) {
        return false;
    }
    *pStream << static_cast<quint16>(utf8.size());
    if (pStream->writeRawData(utf8.constData(), utf8.size()) != utf8.size()) {
        return false;
    }
    return true;
}

bool readString(QDataStream* pStream, QString* pValue) {
    quint16 length = 0;
    *pStream >> length;
    if (pStream->status() != QDataStream::Ok || length == 0) {
        return false;
    }
    QByteArray utf8;
    utf8.resize(length);
    if (pStream->readRawData(utf8.data(), length) != length) {
        return false;
    }
    const QString value = QString::fromUtf8(utf8);
    if (value.isEmpty()) {
        return false;
    }
    *pValue = value;
    return true;
}

void setError(QString* pError, const QString& error) {
    if (pError != nullptr) {
        *pError = error;
    }
}

} // namespace

CollabControlMap CollabControlMap::fromKeys(QList<ConfigKey> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    CollabControlMap map;
    for (const ConfigKey& key : keys) {
        map.add(key);
    }
    return map;
}

bool CollabControlMap::add(const ConfigKey& key) {
    if (!key.isValid() || m_idsByKey.contains(key)) {
        return false;
    }
    const auto nextId = static_cast<CollabControlId>(m_entries.size() + 1);
    if (nextId == kInvalidCollabControlId ||
            m_entries.size() >= std::numeric_limits<CollabControlId>::max()) {
        return false;
    }
    m_entries.append(Entry{nextId, key});
    m_idsByKey.insert(key, nextId);
    return true;
}

std::optional<CollabControlId> CollabControlMap::idFor(
        const ConfigKey& key) const {
    const auto it = m_idsByKey.constFind(key);
    if (it == m_idsByKey.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

std::optional<ConfigKey> CollabControlMap::keyFor(CollabControlId id) const {
    if (id == kInvalidCollabControlId) {
        return std::nullopt;
    }
    for (const Entry& entry : m_entries) {
        if (entry.id == id) {
            return entry.key;
        }
    }
    return std::nullopt;
}

QByteArray CollabControlMap::toByteArray() const {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_6_0);

    stream << kControlMapVersion;
    stream << static_cast<quint16>(m_entries.size());
    for (const Entry& entry : m_entries) {
        stream << entry.id;
        writeString(&stream, entry.key.group);
        writeString(&stream, entry.key.item);
    }
    return data;
}

std::optional<CollabControlMap> CollabControlMap::fromByteArray(
        const QByteArray& data, QString* pError) {
    QDataStream stream(data);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setVersion(QDataStream::Qt_6_0);

    quint8 version = 0;
    quint16 count = 0;
    stream >> version >> count;
    if (stream.status() != QDataStream::Ok || version != kControlMapVersion) {
        setError(pError, QStringLiteral("unsupported control map version"));
        return std::nullopt;
    }

    CollabControlMap map;
    QSet<CollabControlId> seenIds;
    QSet<ConfigKey> seenKeys;
    map.m_entries.reserve(count);
    for (quint16 i = 0; i < count; ++i) {
        CollabControlId id = kInvalidCollabControlId;
        QString group;
        QString item;
        stream >> id;
        if (stream.status() != QDataStream::Ok ||
                !readString(&stream, &group) ||
                !readString(&stream, &item)) {
            setError(pError, QStringLiteral("truncated control map entry"));
            return std::nullopt;
        }
        if (id == kInvalidCollabControlId) {
            setError(pError, QStringLiteral("invalid control id"));
            return std::nullopt;
        }
        if (seenIds.contains(id)) {
            setError(pError, QStringLiteral("duplicate control id"));
            return std::nullopt;
        }
        const ConfigKey key(std::move(group), std::move(item));
        if (seenKeys.contains(key)) {
            setError(pError, QStringLiteral("duplicate control key"));
            return std::nullopt;
        }
        seenIds.insert(id);
        seenKeys.insert(key);
        map.m_entries.append(Entry{id, key});
        map.m_idsByKey.insert(key, id);
    }

    if (!stream.atEnd()) {
        setError(pError, QStringLiteral("trailing control map data"));
        return std::nullopt;
    }

    return map;
}

QList<ConfigKey> collabDefaultControlKeys() {
    const QStringList items = {
            QStringLiteral("play"),
            QStringLiteral("cue_default"),
            QStringLiteral("cue_gotoandstop"),
            QStringLiteral("rate_ratio"),
            QStringLiteral("reverse"),
            QStringLiteral("keylock"),
            QStringLiteral("loop_enabled"),
            QStringLiteral("loop_start_position"),
            QStringLiteral("loop_end_position"),
            QStringLiteral("volume"),
            QStringLiteral("pregain"),
            QStringLiteral("mute"),
            // Crossfader side assignment. Each peer's decks contribute to the
            // host-owned crossfader according to this value, so it has to match
            // on both machines.
            QStringLiteral("orientation"),
            QStringLiteral("filterLow"),
            QStringLiteral("filterMid"),
            QStringLiteral("filterHigh"),
            QStringLiteral("filterLowKill"),
            QStringLiteral("filterMidKill"),
            QStringLiteral("filterHighKill"),
    };
    QList<ConfigKey> keys;
    for (int deck = 1; deck <= kCollabSessionDeckCount; ++deck) {
        const QString group = QStringLiteral("[Channel%1]").arg(deck);
        for (const QString& item : items) {
            keys.append(ConfigKey(group, item));
        }
        for (int hotcue = 1; hotcue <= 8; ++hotcue) {
            keys.append(ConfigKey(
                    group, QStringLiteral("hotcue_%1_activate").arg(hotcue)));
        }
    }
    keys.append(ConfigKey(QStringLiteral("[Master]"),
            QStringLiteral("crossfader")));

    // Standard effect units: four units with four effect slots each. Every
    // unit routes all session decks, so the routing toggles are replicated
    // too. The mix is normalized and the super knob drives every slot's meta
    // parameter, which the replicator applies in dependency order.
    for (int unit = 1; unit <= kNumStandardEffectUnits; ++unit) {
        QStringList routingDeckGroups;
        for (int deck = 1; deck <= kCollabSessionDeckCount; ++deck) {
            routingDeckGroups.append(QStringLiteral("[Channel%1]").arg(deck));
        }
        appendEffectChainKeys(&keys,
                QStringLiteral("[EffectRack1_EffectUnit%1]").arg(unit),
                routingDeckGroups);
    }

    // Per-deck quick effect chain behind the mixer filter knob. It has four
    // slots like a standard unit but only routes its own deck.
    for (int deck = 1; deck <= kCollabSessionDeckCount; ++deck) {
        const QString deckGroup = QStringLiteral("[Channel%1]").arg(deck);
        appendEffectChainKeys(&keys,
                QStringLiteral("[QuickEffectRack1_%1]").arg(deckGroup),
                {deckGroup});
    }

    return keys;
}

ConfigKey collabLocalControlKey(
        const ConfigKey& sessionKey, const CollabDeckMap& deckMap) {
    const QString& group = sessionKey.group;
    if (group.size() == QStringLiteral("[Channel1]").size() &&
            group.startsWith(QStringLiteral("[Channel")) &&
            group.endsWith(QLatin1Char(']'))) {
        bool ok = false;
        const int sessionDeck = group.mid(8, 1).toInt(&ok);
        if (ok && isValidCollabDeckId(sessionDeck)) {
            return ConfigKey(deckMap.localGroupForSessionDeck(sessionDeck),
                    sessionKey.item);
        }
    }
    return sessionKey;
}

} // namespace collab
} // namespace mixxx
