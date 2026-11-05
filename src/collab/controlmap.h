#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <optional>

#include "collab/collabtypes.h"
#include "preferences/configobject.h"

namespace mixxx {
namespace collab {

/// Session-local mapping between compact control identifiers and ConfigKeys.
///
/// Identifiers are assigned in sorted ConfigKey order so both peers produce
/// the same map from the same control set. The map is exchanged as a whole
/// during the join handshake. A receiver must reject controls that are not in
/// the negotiated map instead of guessing what they refer to.
class CollabControlMap {
  public:
    struct Entry {
        CollabControlId id;
        ConfigKey key;
    };

    CollabControlMap() = default;

    /// Builds a map from the given keys. Sorts and deduplicates the keys.
    static CollabControlMap fromKeys(QList<ConfigKey> keys);

    /// Appends a key with the next free identifier. Returns false when the key
    /// is invalid or already present.
    bool add(const ConfigKey& key);

    bool isEmpty() const {
        return m_entries.isEmpty();
    }

    int size() const {
        return static_cast<int>(m_entries.size());
    }

    QList<Entry> entries() const {
        return m_entries;
    }

    std::optional<CollabControlId> idFor(const ConfigKey& key) const;
    std::optional<ConfigKey> keyFor(CollabControlId id) const;

    /// Serializes the map for the join handshake.
    QByteArray toByteArray() const;

    /// Parses a map received from the peer. Returns std::nullopt and sets
    /// pError when the payload is malformed or ambiguous.
    static std::optional<CollabControlMap> fromByteArray(
            const QByteArray& data, QString* pError = nullptr);

  private:
    QList<Entry> m_entries;
    QHash<ConfigKey, CollabControlId> m_idsByKey;
};

} // namespace collab
} // namespace mixxx
