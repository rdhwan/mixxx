#include "collab/tracktransfer.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <utility>

#include "moc_tracktransfer.cpp"

namespace mixxx {
namespace collab {

namespace {

constexpr quint32 kTransferMagic = 0x4D585854; // "MXXT"
constexpr quint16 kTransferVersion = 1;
constexpr int kRequestSize = 4 + 2 + CollabContentHash::kSize + 8;
constexpr int kResponseFixedSize = 4 + 2 + 1 + 8 + 2;
constexpr int kChunkBytes = 32 * 1024;
constexpr qint64 kSendWindowBytes = 512 * 1024;
constexpr int kTransferTimeoutMs = 15000;
constexpr qint64 kProgressStepBytes = 256 * 1024;
constexpr int kMaxConcurrentFetches = 4;

enum class TransferStatus : quint8 {
    Ok = 0,
    NotFound = 1,
    BadRequest = 2,
    BadOffset = 3,
};

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

QByteArray encodeTransferRequest(const CollabContentHash& hash, qint64 offset) {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(kTransferMagic) << quint16(kTransferVersion);
    stream.writeRawData(hash.bytes.constData(), CollabContentHash::kSize);
    stream << quint64(offset);
    return data;
}

QByteArray encodeTransferResponse(
        TransferStatus status, qint64 totalBytes, const QString& message) {
    const QByteArray messageBytes = message.toUtf8();
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(kTransferMagic) << quint16(kTransferVersion)
           << quint8(status) << quint64(totalBytes)
           << quint16(messageBytes.size());
    if (!messageBytes.isEmpty()) {
        stream.writeRawData(messageBytes.constData(), messageBytes.size());
    }
    return data;
}

QString statusMessage(TransferStatus status) {
    switch (status) {
    case TransferStatus::Ok:
        return QString();
    case TransferStatus::NotFound:
        return QStringLiteral("The peer does not have this track.");
    case TransferStatus::BadRequest:
        return QStringLiteral("The peer rejected the transfer request.");
    case TransferStatus::BadOffset:
        return QStringLiteral("The peer rejected the resume offset.");
    }
    return QStringLiteral("The peer rejected the transfer.");
}

} // namespace

struct CollabTransferService::Fetch {
    QTcpSocket* pSocket = nullptr;
    QFile file;
    CollabContentHash hash;
    QString suffix;
    QString partialPath;
    QByteArray headerBuffer;
    bool headerDone = false;
    qint64 total = -1;
    qint64 received = 0;
    qint64 lastProgressEmit = 0;
    QTimer* pTimeout = nullptr;
    bool finished = false;
};

struct CollabTransferService::Serve {
    QTcpSocket* pSocket = nullptr;
    QFile file;
    CollabContentHash hash;
    QString path;
    QByteArray headerBuffer;
    bool headerDone = false;
    qint64 total = 0;
    qint64 sent = 0;
    qint64 lastProgressEmit = 0;
    QTimer* pTimeout = nullptr;
    bool finished = false;
};

CollabTransferService::CollabTransferService(
        CollabTrackCache* pCache, QObject* pParent)
        : QObject(pParent),
          m_pCache(pCache),
          m_pServer(new QTcpServer(this)) {
    connect(m_pServer,
            &QTcpServer::newConnection,
            this,
            &CollabTransferService::onNewConnection);
}

CollabTransferService::~CollabTransferService() {
    close();
}

QString CollabTransferService::hashKey(const CollabContentHash& hash) {
    return hash.toHex();
}

bool CollabTransferService::listen(quint16 port, QString* pError) {
    if (m_pCache == nullptr) {
        setError(pError, QStringLiteral("The track cache is not available."));
        return false;
    }
    if (m_pServer->isListening()) {
        m_pServer->close();
    }
    if (!m_pServer->listen(QHostAddress::Any, port)) {
        setError(pError, m_pServer->errorString());
        return false;
    }
    return true;
}

void CollabTransferService::close() {
    if (m_pServer != nullptr) {
        m_pServer->close();
    }
    while (!m_fetches.empty()) {
        auto it = m_fetches.begin();
        QTcpSocket* pSocket = it->first;
        it->second->finished = true;
        it->second->file.close();
        m_fetches.erase(it);
        pSocket->abort();
        pSocket->deleteLater();
    }
    while (!m_serves.empty()) {
        auto it = m_serves.begin();
        QTcpSocket* pSocket = it->first;
        it->second->finished = true;
        it->second->file.close();
        m_serves.erase(it);
        pSocket->abort();
        pSocket->deleteLater();
    }
}

bool CollabTransferService::isListening() const {
    return m_pServer->isListening();
}

quint16 CollabTransferService::localPort() const {
    return m_pServer->serverPort();
}

void CollabTransferService::setAllowedPeer(const QHostAddress& address) {
    m_allowedPeer = address;
}

void CollabTransferService::serveFile(
        const CollabContentHash& hash, const QString& path) {
    if (!hash.isValid() || path.isEmpty()) {
        return;
    }
    m_servedFiles.insert(hashKey(hash), path);
}

void CollabTransferService::removeServedFile(const CollabContentHash& hash) {
    m_servedFiles.remove(hashKey(hash));
}

bool CollabTransferService::fetch(const QHostAddress& peer,
        quint16 port,
        const CollabContentHash& hash,
        const QString& suffix,
        QString* pError) {
    if (m_pCache == nullptr) {
        setError(pError, QStringLiteral("The track cache is not available."));
        return false;
    }
    if (!hash.isValid()) {
        setError(pError, QStringLiteral("The content hash is invalid."));
        return false;
    }
    if (port == 0) {
        setError(pError, QStringLiteral("The peer transfer port is zero."));
        return false;
    }
    if (isFetching(hash)) {
        setError(pError, QStringLiteral("That track is already being received."));
        return false;
    }
    if (m_fetches.size() >= kMaxConcurrentFetches) {
        setError(pError, QStringLiteral("Too many transfers are already running."));
        return false;
    }
    if (!m_pCache->ensureRoot(pError)) {
        return false;
    }

    const QString partialPath = m_pCache->partialPathForHash(hash);
    if (partialPath.isEmpty()) {
        setError(pError, QStringLiteral("Cannot build the partial file path."));
        return false;
    }
    if (!QDir().mkpath(QFileInfo(partialPath).absolutePath())) {
        setError(pError, QStringLiteral("Cannot create the partial download directory."));
        return false;
    }

    auto pFetch = std::make_unique<Fetch>();
    pFetch->hash = hash;
    pFetch->suffix = suffix;
    pFetch->partialPath = partialPath;
    pFetch->file.setFileName(partialPath);
    if (!pFetch->file.open(QIODevice::ReadWrite)) {
        setError(pError, pFetch->file.errorString());
        return false;
    }
    const qint64 offset = pFetch->file.size();
    if (!pFetch->file.seek(offset)) {
        setError(pError, pFetch->file.errorString());
        return false;
    }
    pFetch->received = offset;
    pFetch->lastProgressEmit = offset;

    auto* pSocket = new QTcpSocket(this);
    pFetch->pSocket = pSocket;
    pFetch->pTimeout = new QTimer(pSocket);
    pFetch->pTimeout->setSingleShot(true);
    pFetch->pTimeout->start(kTransferTimeoutMs);
    connect(pFetch->pTimeout, &QTimer::timeout, this, [this, pSocket]() {
        auto it = m_fetches.find(pSocket);
        if (it != m_fetches.end()) {
            failFetch(it->second.get(), QStringLiteral("The transfer timed out."));
        }
    });
    connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket]() {
        onFetchReadyRead(pSocket);
    });
    connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket]() {
        onFetchDisconnected(pSocket);
    });
    connect(pSocket,
            &QTcpSocket::errorOccurred,
            this,
            [this, pSocket](QAbstractSocket::SocketError) {
                auto it = m_fetches.find(pSocket);
                if (it != m_fetches.end()) {
                    failFetch(it->second.get(), pSocket->errorString());
                }
            });

    const QByteArray request = encodeTransferRequest(hash, offset);
    connect(pSocket, &QTcpSocket::connected, pSocket, [pSocket, request]() {
        pSocket->write(request);
    });

    m_fetches.emplace(pSocket, std::move(pFetch));
    pSocket->connectToHost(peer, port);
    return true;
}

void CollabTransferService::cancelFetch(const CollabContentHash& hash) {
    for (auto it = m_fetches.begin(); it != m_fetches.end(); ++it) {
        if (it->second->hash == hash) {
            failFetch(it->second.get(), QStringLiteral("The transfer was cancelled."));
            return;
        }
    }
}

bool CollabTransferService::isFetching(const CollabContentHash& hash) const {
    for (const auto& fetch : m_fetches) {
        if (fetch.second->hash == hash) {
            return true;
        }
    }
    return false;
}

void CollabTransferService::onNewConnection() {
    while (m_pServer->hasPendingConnections()) {
        QTcpSocket* pSocket = m_pServer->nextPendingConnection();
        if (!m_allowedPeer.isNull() &&
                !pSocket->peerAddress().isEqual(m_allowedPeer)) {
            pSocket->abort();
            pSocket->deleteLater();
            continue;
        }

        auto pServe = std::make_unique<Serve>();
        pServe->pSocket = pSocket;
        pServe->pTimeout = new QTimer(pSocket);
        pServe->pTimeout->setSingleShot(true);
        pServe->pTimeout->start(kTransferTimeoutMs);
        connect(pServe->pTimeout, &QTimer::timeout, this, [this, pSocket]() {
            auto it = m_serves.find(pSocket);
            if (it != m_serves.end()) {
                failServe(it->second.get(), QStringLiteral("The transfer timed out."));
            }
        });
        connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket]() {
            onServeReadyRead(pSocket);
        });
        connect(pSocket, &QTcpSocket::bytesWritten, this, [this, pSocket]() {
            auto it = m_serves.find(pSocket);
            if (it != m_serves.end()) {
                pumpServe(it->second.get());
            }
        });
        connect(pSocket, &QTcpSocket::disconnected, this, [this, pSocket]() {
            auto it = m_serves.find(pSocket);
            if (it != m_serves.end()) {
                Serve* pServe = it->second.get();
                if (pServe->finished) {
                    m_serves.erase(it);
                    pSocket->deleteLater();
                } else {
                    failServe(pServe, QStringLiteral("The connection closed."));
                }
            }
        });
        connect(pSocket,
                &QTcpSocket::errorOccurred,
                this,
                [this, pSocket](QAbstractSocket::SocketError) {
                    auto it = m_serves.find(pSocket);
                    if (it != m_serves.end()) {
                        failServe(it->second.get(), pSocket->errorString());
                    }
                });
        m_serves.emplace(pSocket, std::move(pServe));
    }
}

void CollabTransferService::onFetchReadyRead(QTcpSocket* pSocket) {
    auto it = m_fetches.find(pSocket);
    if (it == m_fetches.end()) {
        return;
    }
    Fetch* pFetch = it->second.get();
    pFetch->pTimeout->start(kTransferTimeoutMs);
    const QByteArray data = pSocket->readAll();

    if (pFetch->headerDone) {
        writeFetchedData(pFetch, data);
        return;
    }

    pFetch->headerBuffer.append(data);
    if (pFetch->headerBuffer.size() < kResponseFixedSize) {
        return;
    }

    QDataStream stream(pFetch->headerBuffer);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint16 version = 0;
    quint8 status = 0;
    quint64 totalBytes = 0;
    quint16 messageLength = 0;
    stream >> magic >> version >> status >> totalBytes >> messageLength;
    if (stream.status() != QDataStream::Ok ||
            magic != kTransferMagic ||
            version != kTransferVersion) {
        failFetch(pFetch, QStringLiteral("The peer sent an invalid transfer response."));
        return;
    }
    if (pFetch->headerBuffer.size() < kResponseFixedSize + messageLength) {
        return;
    }

    QString message;
    if (messageLength > 0) {
        QByteArray messageBytes(messageLength, Qt::Uninitialized);
        stream.readRawData(messageBytes.data(), messageLength);
        message = QString::fromUtf8(messageBytes);
    }
    const TransferStatus transferStatus = static_cast<TransferStatus>(status);
    if (transferStatus != TransferStatus::Ok) {
        failFetch(pFetch,
                message.isEmpty() ? statusMessage(transferStatus) : message);
        return;
    }
    if (totalBytes >
            static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        failFetch(pFetch, QStringLiteral("The peer reported an invalid file size."));
        return;
    }
    const qint64 total = static_cast<qint64>(totalBytes);
    if (total < pFetch->received) {
        failFetch(pFetch, QStringLiteral("The peer reported a file smaller than the partial copy."));
        return;
    }
    if (!m_pCache->hasSpaceFor(total - pFetch->received)) {
        failFetch(pFetch, QStringLiteral("Not enough disk space for this track."));
        return;
    }

    pFetch->total = total;
    pFetch->headerDone = true;
    const QByteArray remaining =
            pFetch->headerBuffer.mid(kResponseFixedSize + messageLength);
    pFetch->headerBuffer.clear();
    emit fetchProgress(pFetch->hash, pFetch->received, pFetch->total);
    if (pFetch->received >= pFetch->total) {
        // An empty file, or a partial copy that already covers the whole file,
        // has nothing left to receive.
        finishFetch(pFetch);
        return;
    }
    if (!remaining.isEmpty()) {
        writeFetchedData(pFetch, remaining);
    }
}

void CollabTransferService::onFetchDisconnected(QTcpSocket* pSocket) {
    auto it = m_fetches.find(pSocket);
    if (it == m_fetches.end()) {
        return;
    }
    Fetch* pFetch = it->second.get();
    if (pFetch->finished) {
        return;
    }
    failFetch(pFetch, QStringLiteral("The connection closed before the track was complete."));
}

void CollabTransferService::writeFetchedData(
        Fetch* pFetch, const QByteArray& data) {
    if (pFetch->finished) {
        return;
    }
    const qint64 remaining = pFetch->total - pFetch->received;
    if (remaining <= 0) {
        finishFetch(pFetch);
        return;
    }
    if (data.isEmpty()) {
        return;
    }
    const qint64 toWrite = std::min<qint64>(remaining, data.size());
    const qint64 written = pFetch->file.write(data.constData(), toWrite);
    if (written != toWrite) {
        failFetch(pFetch, pFetch->file.errorString());
        return;
    }
    pFetch->received += written;
    if (pFetch->received - pFetch->lastProgressEmit >= kProgressStepBytes ||
            pFetch->received == pFetch->total) {
        pFetch->lastProgressEmit = pFetch->received;
        emit fetchProgress(pFetch->hash, pFetch->received, pFetch->total);
    }
    if (pFetch->received >= pFetch->total) {
        finishFetch(pFetch);
    }
}

void CollabTransferService::failFetch(Fetch* pFetch, const QString& error) {
    if (pFetch->finished) {
        return;
    }
    pFetch->finished = true;
    pFetch->pTimeout->stop();
    pFetch->file.close();
    const CollabContentHash hash = pFetch->hash;
    QTcpSocket* pSocket = pFetch->pSocket;
    m_fetches.erase(pSocket);
    pSocket->abort();
    pSocket->deleteLater();
    emit fetchFailed(hash, error);
}

void CollabTransferService::finishFetch(Fetch* pFetch) {
    if (pFetch->finished) {
        return;
    }
    pFetch->finished = true;
    pFetch->pTimeout->stop();
    pFetch->file.close();
    const CollabContentHash hash = pFetch->hash;
    const QString partialPath = pFetch->partialPath;
    const QString suffix = pFetch->suffix;
    QTcpSocket* pSocket = pFetch->pSocket;
    m_fetches.erase(pSocket);
    pSocket->abort();
    pSocket->deleteLater();

    QString error;
    if (!m_pCache->install(hash, partialPath, suffix, &error)) {
        emit fetchFailed(hash, error);
        return;
    }
    const auto installed = m_pCache->find(hash);
    if (!installed.has_value()) {
        emit fetchFailed(hash, QStringLiteral("The verified file is not in the cache."));
        return;
    }
    emit fetchFinished(hash, *installed);
}

void CollabTransferService::onServeReadyRead(QTcpSocket* pSocket) {
    auto it = m_serves.find(pSocket);
    if (it == m_serves.end()) {
        return;
    }
    Serve* pServe = it->second.get();
    pServe->pTimeout->start(kTransferTimeoutMs);
    if (pServe->headerDone) {
        pSocket->readAll();
        return;
    }
    pServe->headerBuffer.append(pSocket->readAll());
    if (pServe->headerBuffer.size() < kRequestSize) {
        return;
    }

    QDataStream stream(pServe->headerBuffer);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 magic = 0;
    quint16 version = 0;
    QByteArray hashBytes(CollabContentHash::kSize, Qt::Uninitialized);
    quint64 offset = 0;
    stream >> magic >> version;
    const int hashRead = stream.readRawData(hashBytes.data(), hashBytes.size());
    stream >> offset;
    if (stream.status() != QDataStream::Ok ||
            magic != kTransferMagic ||
            version != kTransferVersion ||
            hashRead != hashBytes.size()) {
        pSocket->write(encodeTransferResponse(
                TransferStatus::BadRequest, 0, QStringLiteral("Invalid request.")));
        pServe->finished = true;
        pServe->pTimeout->stop();
        pSocket->disconnectFromHost();
        return;
    }

    const CollabContentHash hash{hashBytes};
    const auto pathIt = m_servedFiles.constFind(hashKey(hash));
    if (pathIt == m_servedFiles.constEnd()) {
        pSocket->write(encodeTransferResponse(TransferStatus::NotFound, 0, QString()));
        pServe->finished = true;
        pServe->pTimeout->stop();
        pSocket->disconnectFromHost();
        return;
    }

    const QFileInfo info(pathIt.value());
    if (!info.exists() || !info.isFile()) {
        pSocket->write(encodeTransferResponse(TransferStatus::NotFound, 0, QString()));
        pServe->finished = true;
        pServe->pTimeout->stop();
        pSocket->disconnectFromHost();
        return;
    }
    const qint64 total = info.size();
    if (offset > static_cast<quint64>(total)) {
        pSocket->write(encodeTransferResponse(TransferStatus::BadOffset, total, QString()));
        pServe->finished = true;
        pServe->pTimeout->stop();
        pSocket->disconnectFromHost();
        return;
    }

    pServe->file.setFileName(info.absoluteFilePath());
    if (!pServe->file.open(QIODevice::ReadOnly) ||
            !pServe->file.seek(static_cast<qint64>(offset))) {
        pSocket->write(encodeTransferResponse(
                TransferStatus::BadRequest, total, pServe->file.errorString()));
        pServe->finished = true;
        pServe->pTimeout->stop();
        pSocket->disconnectFromHost();
        return;
    }

    pServe->hash = hash;
    pServe->path = info.absoluteFilePath();
    pServe->total = total;
    pServe->sent = static_cast<qint64>(offset);
    pServe->lastProgressEmit = pServe->sent;
    pServe->headerDone = true;
    pServe->headerBuffer.clear();
    pSocket->write(encodeTransferResponse(TransferStatus::Ok, total, QString()));
    emit serveProgress(hash, pServe->sent, pServe->total);
    pumpServe(pServe);
}

void CollabTransferService::pumpServe(Serve* pServe) {
    if (pServe->finished) {
        return;
    }
    pServe->pTimeout->start(kTransferTimeoutMs);
    while (pServe->pSocket->bytesToWrite() < kSendWindowBytes) {
        const QByteArray chunk = pServe->file.read(kChunkBytes);
        if (chunk.isEmpty()) {
            if (pServe->file.error() != QFileDevice::NoError) {
                failServe(pServe, pServe->file.errorString());
                return;
            }
            finishServe(pServe);
            return;
        }
        if (pServe->pSocket->write(chunk) != chunk.size()) {
            failServe(pServe, pServe->pSocket->errorString());
            return;
        }
        pServe->sent += chunk.size();
        if (pServe->sent - pServe->lastProgressEmit >= kProgressStepBytes ||
                pServe->sent == pServe->total) {
            pServe->lastProgressEmit = pServe->sent;
            emit serveProgress(pServe->hash, pServe->sent, pServe->total);
        }
    }
}

void CollabTransferService::finishServe(Serve* pServe) {
    if (pServe->finished) {
        return;
    }
    pServe->finished = true;
    pServe->pTimeout->stop();
    pServe->file.close();
    emit serveFinished(pServe->hash);
    pServe->pSocket->disconnectFromHost();
}

void CollabTransferService::failServe(Serve* pServe, const QString& error) {
    if (pServe->finished) {
        return;
    }
    pServe->finished = true;
    pServe->pTimeout->stop();
    pServe->file.close();
    const CollabContentHash hash = pServe->hash;
    QTcpSocket* pSocket = pServe->pSocket;
    m_serves.erase(pSocket);
    pSocket->abort();
    pSocket->deleteLater();
    if (hash.isValid()) {
        emit serveFailed(hash, error);
    }
}

} // namespace collab
} // namespace mixxx
