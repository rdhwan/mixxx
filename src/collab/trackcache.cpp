#include "collab/trackcache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <utility>

namespace mixxx {
namespace collab {

namespace {

constexpr char kPartialDirectory[] = "partial";

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

} // namespace

CollabTrackCache::CollabTrackCache(QString root)
        : m_root(std::move(root)) {
}

bool CollabTrackCache::ensureRoot(QString* pError) const {
    if (m_root.isEmpty()) {
        setError(pError, QStringLiteral("The track cache path is empty."));
        return false;
    }
    if (!QDir().mkpath(m_root)) {
        setError(pError, QStringLiteral("Cannot create the track cache directory."));
        return false;
    }
    if (!QDir().mkpath(QDir(m_root).filePath(QString::fromLatin1(kPartialDirectory)))) {
        setError(pError,
                QStringLiteral("Cannot create the partial download directory."));
        return false;
    }
    return true;
}

QString CollabTrackCache::normalizedSuffix(const QString& suffix) {
    QString value = suffix.trimmed();
    while (value.startsWith(QLatin1Char('.'))) {
        value.remove(0, 1);
    }
    if (value.isEmpty()) {
        return QString();
    }
    static const QRegularExpression validSuffix(
            QStringLiteral("^[A-Za-z0-9]{1,12}$"));
    if (!validSuffix.match(value).hasMatch()) {
        return QString();
    }
    return QLatin1Char('.') + value.toLower();
}

QString CollabTrackCache::directoryForHash(const CollabContentHash& hash) const {
    if (!hash.isValid()) {
        return QString();
    }
    const QString hex = hash.toHex();
    return QDir(m_root).filePath(hex.left(2));
}

QString CollabTrackCache::pathForHash(
        const CollabContentHash& hash, const QString& suffix) const {
    if (!hash.isValid()) {
        return QString();
    }
    return QDir(directoryForHash(hash))
            .filePath(hash.toHex() + normalizedSuffix(suffix));
}

std::optional<QString> CollabTrackCache::find(const CollabContentHash& hash) const {
    if (!hash.isValid()) {
        return std::nullopt;
    }
    const QString hex = hash.toHex();
    const QDir directory(directoryForHash(hash));
    if (!directory.exists()) {
        return std::nullopt;
    }
    const QFileInfoList files = directory.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& file : files) {
        const QString name = file.fileName();
        if (name == hex || name.startsWith(hex + QLatin1Char('.'))) {
            return file.absoluteFilePath();
        }
    }
    return std::nullopt;
}

QString CollabTrackCache::partialPathForHash(const CollabContentHash& hash) const {
    if (!hash.isValid()) {
        return QString();
    }
    return QDir(QDir(m_root).filePath(QString::fromLatin1(kPartialDirectory)))
            .filePath(hash.toHex() + QStringLiteral(".part"));
}

bool CollabTrackCache::install(const CollabContentHash& hash,
        const QString& partialPath,
        const QString& suffix,
        QString* pError) {
    if (!hash.isValid()) {
        setError(pError, QStringLiteral("The expected content hash is invalid."));
        return false;
    }
    if (!QFileInfo::exists(partialPath)) {
        setError(pError, QStringLiteral("The partial download does not exist."));
        return false;
    }
    if (!verify(hash, partialPath, pError)) {
        QFile::remove(partialPath);
        return false;
    }

    const QString finalPath = pathForHash(hash, suffix);
    if (finalPath.isEmpty()) {
        setError(pError, QStringLiteral("Cannot build the cache path."));
        return false;
    }
    if (QFileInfo::exists(finalPath)) {
        // The same content is already cached; the partial copy is redundant.
        QFile::remove(partialPath);
        return true;
    }
    if (!QDir().mkpath(QFileInfo(finalPath).absolutePath())) {
        setError(pError, QStringLiteral("Cannot create the cache directory."));
        return false;
    }
    if (!QFile::rename(partialPath, finalPath)) {
        setError(pError, QStringLiteral("Cannot move the verified file into the cache."));
        return false;
    }
    return true;
}

bool CollabTrackCache::verify(const CollabContentHash& hash,
        const QString& path,
        QString* pError) const {
    if (!hash.isValid()) {
        setError(pError, QStringLiteral("The expected content hash is invalid."));
        return false;
    }
    CollabContentHash actual;
    if (!collabHashFile(path, &actual, pError)) {
        return false;
    }
    if (actual != hash) {
        setError(pError, QStringLiteral("The file contents do not match the hash."));
        return false;
    }
    return true;
}

qint64 CollabTrackCache::availableBytes() const {
    QStorageInfo storage(m_root);
    if (!storage.isValid()) {
        storage.setPath(QFileInfo(m_root).absolutePath());
    }
    if (!storage.isValid() || storage.bytesAvailable() < 0) {
        return -1;
    }
    return storage.bytesAvailable();
}

bool CollabTrackCache::hasSpaceFor(qint64 bytes) const {
    if (bytes < 0) {
        return false;
    }
    const qint64 available = availableBytes();
    if (available < 0) {
        // Unknown free space is not a reason to reject a transfer; the write
        // itself will fail if the disk is full.
        return true;
    }
    return available >= bytes;
}

} // namespace collab
} // namespace mixxx
