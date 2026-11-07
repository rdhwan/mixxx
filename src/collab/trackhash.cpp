#include "collab/trackhash.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <utility>

namespace mixxx {
namespace collab {

namespace {

constexpr int kReadChunkBytes = 1 << 20;
constexpr int kIndexVersion = 1;
constexpr char kIndexVersionKey[] = "version";
constexpr char kIndexEntriesKey[] = "entries";
constexpr char kEntryPathKey[] = "path";
constexpr char kEntryHashKey[] = "sha256";
constexpr char kEntrySizeKey[] = "size";
constexpr char kEntryModifiedKey[] = "mtimeMs";

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

} // namespace

QString CollabContentHash::toHex() const {
    return QString::fromLatin1(bytes.toHex());
}

std::optional<CollabContentHash> CollabContentHash::fromHex(const QString& hex) {
    const QByteArray decoded = QByteArray::fromHex(hex.toLatin1());
    if (decoded.size() != kSize) {
        return std::nullopt;
    }
    return CollabContentHash{decoded};
}

bool collabHashFile(const QString& path,
        CollabContentHash* pHash,
        QString* pError) {
    if (pHash == nullptr) {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(pError, file.errorString());
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(kReadChunkBytes, Qt::Uninitialized);
    while (!file.atEnd()) {
        const qint64 read = file.read(buffer.data(), buffer.size());
        if (read < 0) {
            setError(pError, file.errorString());
            return false;
        }
        if (read == 0) {
            break;
        }
        hash.addData(QByteArrayView(buffer.constData(), read));
    }
    if (file.error() != QFileDevice::NoError) {
        setError(pError, file.errorString());
        return false;
    }

    pHash->bytes = hash.result();
    return true;
}

QString CollabTrackHashIndex::normalizedPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

std::optional<CollabContentHash> CollabTrackHashIndex::lookup(
        const QString& path) const {
    const auto it = m_entries.constFind(normalizedPath(path));
    if (it == m_entries.constEnd()) {
        return std::nullopt;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return std::nullopt;
    }
    if (info.size() != it->size ||
            info.lastModified().toMSecsSinceEpoch() != it->modifiedMs) {
        return std::nullopt;
    }
    return it->hash;
}

bool CollabTrackHashIndex::insert(
        const QString& path, const CollabContentHash& hash) {
    if (!hash.isValid()) {
        return false;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    Entry entry;
    entry.hash = hash;
    entry.size = info.size();
    entry.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    m_entries.insert(normalizedPath(path), entry);
    return true;
}

void CollabTrackHashIndex::remove(const QString& path) {
    m_entries.remove(normalizedPath(path));
}

void CollabTrackHashIndex::clear() {
    m_entries.clear();
}

bool CollabTrackHashIndex::load(const QString& path, QString* pError) {
    QFile file(path);
    if (!file.exists()) {
        m_entries.clear();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setError(pError, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(pError, parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        setError(pError, QStringLiteral("The hash index is not a JSON object."));
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(kIndexVersionKey).toInt() != kIndexVersion) {
        setError(pError, QStringLiteral("Unsupported hash index version."));
        return false;
    }

    QHash<QString, Entry> entries;
    const QJsonArray array = root.value(kIndexEntriesKey).toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QString entryPath = object.value(kEntryPathKey).toString();
        const auto hash = CollabContentHash::fromHex(
                object.value(kEntryHashKey).toString());
        const qint64 size = static_cast<qint64>(object.value(kEntrySizeKey).toDouble());
        const qint64 modifiedMs =
                static_cast<qint64>(object.value(kEntryModifiedKey).toDouble());
        if (entryPath.isEmpty() || !hash.has_value()) {
            continue;
        }
        entries.insert(entryPath, Entry{*hash, size, modifiedMs});
    }
    m_entries = std::move(entries);
    return true;
}

bool CollabTrackHashIndex::save(const QString& path, QString* pError) const {
    QJsonArray array;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        QJsonObject object;
        object.insert(kEntryPathKey, it.key());
        object.insert(kEntryHashKey, it->hash.toHex());
        object.insert(kEntrySizeKey, static_cast<double>(it->size));
        object.insert(kEntryModifiedKey, static_cast<double>(it->modifiedMs));
        array.append(object);
    }

    QJsonObject root;
    root.insert(kIndexVersionKey, kIndexVersion);
    root.insert(kIndexEntriesKey, array);

    const QString parent = QFileInfo(path).absolutePath();
    if (!parent.isEmpty() && !QDir().mkpath(parent)) {
        setError(pError, QStringLiteral("Cannot create the hash index directory."));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(pError, file.errorString());
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
        setError(pError, file.errorString());
        return false;
    }
    if (!file.commit()) {
        setError(pError, file.errorString());
        return false;
    }
    return true;
}

} // namespace collab
} // namespace mixxx
