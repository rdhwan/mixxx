#pragma once

#include <QString>
#include <optional>

#include "collab/trackhash.h"

namespace mixxx {
namespace collab {

/// Content-addressed cache for received track files.
///
/// Files are stored under their SHA-256, so two peers that already have the
/// same audio bytes share one cache entry. A partial download lives in a
/// separate directory until the full hash has been verified; only then is it
/// moved into the cache with an atomic rename.
class CollabTrackCache {
  public:
    explicit CollabTrackCache(QString root);

    QString root() const {
        return m_root;
    }

    /// Creates the cache directories. Returns false and sets `pError` when the
    /// path cannot be created.
    bool ensureRoot(QString* pError = nullptr) const;

    /// Final path for `hash` with the given file extension. The path is not
    /// created. Returns an empty string for an invalid hash.
    QString pathForHash(const CollabContentHash& hash, const QString& suffix) const;

    /// Returns the installed file for `hash`, or nullopt when it is absent.
    std::optional<QString> find(const CollabContentHash& hash) const;

    /// Path used while a transfer is in progress. The caller owns the file.
    QString partialPathForHash(const CollabContentHash& hash) const;

    /// Verifies `partialPath` and moves it into the cache. A mismatch deletes
    /// the partial file and fails, so corrupt data never becomes loadable.
    bool install(const CollabContentHash& hash,
            const QString& partialPath,
            const QString& suffix,
            QString* pError = nullptr);

    /// True when `path` exists and its contents hash to `hash`.
    bool verify(const CollabContentHash& hash,
            const QString& path,
            QString* pError = nullptr) const;

    /// Free bytes on the cache volume, or -1 when it cannot be determined.
    qint64 availableBytes() const;
    /// True when the cache volume has room for `bytes` more data.
    bool hasSpaceFor(qint64 bytes) const;

  private:
    QString directoryForHash(const CollabContentHash& hash) const;
    static QString normalizedSuffix(const QString& suffix);

    QString m_root;
};

} // namespace collab
} // namespace mixxx
