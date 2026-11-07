#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <optional>

namespace mixxx {
namespace collab {

/// SHA-256 content hash of a track file.
struct CollabContentHash {
    static constexpr int kSize = 32;

    QByteArray bytes;

    bool isValid() const {
        return bytes.size() == kSize;
    }
    QString toHex() const;
    static std::optional<CollabContentHash> fromHex(const QString& hex);

    bool operator==(const CollabContentHash& other) const {
        return bytes == other.bytes;
    }
    bool operator!=(const CollabContentHash& other) const {
        return !(*this == other);
    }
};

/// Hashes the file contents with SHA-256. Returns false on an I/O error.
bool collabHashFile(const QString& path,
        CollabContentHash* pHash,
        QString* pError = nullptr);

/// Cached path -> content hash, invalidated by size or modification time.
///
/// The index is deliberately dumb: it does not watch files, start work, or
/// decide what to share. The caller hashes a file, inserts the result, and
/// later asks whether the cached hash still describes the file on disk.
class CollabTrackHashIndex {
  public:
    /// Returns the cached hash when the file still matches the recorded size
    /// and modification time. Returns nullopt when the file is missing, changed
    /// or unknown.
    std::optional<CollabContentHash> lookup(const QString& path) const;

    /// Records the hash of an existing file together with its current size and
    /// modification time. Returns false for a missing file or an invalid hash.
    bool insert(const QString& path, const CollabContentHash& hash);

    void remove(const QString& path);
    void clear();
    int size() const {
        return static_cast<int>(m_entries.size());
    }

    /// Loads a previously saved index. A missing file clears the index and
    /// succeeds; malformed JSON fails.
    bool load(const QString& path, QString* pError = nullptr);
    /// Writes the index atomically. Creates the parent directory when needed.
    bool save(const QString& path, QString* pError = nullptr) const;

  private:
    struct Entry {
        CollabContentHash hash;
        qint64 size = 0;
        qint64 modifiedMs = 0;
    };

    static QString normalizedPath(const QString& path);

    QHash<QString, Entry> m_entries;
};

} // namespace collab
} // namespace mixxx
