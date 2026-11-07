#include "collab/collabtrackshare.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QSaveFile>

#include "collab/collabsession.h"
#include "collab/tracktransfer.h"
#include "moc_collabtrackshare.cpp"
#include "util/logger.h"

namespace mixxx {
namespace collab {

namespace {

const Logger kLogger("CollabTrackShare");

constexpr uint16_t kReasonTransferFailed = 2;
constexpr uint16_t kReasonLocalLoadFailed = 3;

void setError(QString* pError, const QString& message) {
    if (pError != nullptr) {
        *pError = message;
    }
}

} // namespace

CollabTrackShare::CollabTrackShare(CollabSession* pSession,
        CollabTransferService* pTransfer,
        CollabTrackHashIndex* pHashIndex,
        CollabTrackCache* pCache,
        QObject* pParent)
        : QObject(pParent),
          m_pSession(pSession),
          m_pTransfer(pTransfer),
          m_pHashIndex(pHashIndex),
          m_pCache(pCache) {
    connect(m_pTransfer,
            &CollabTransferService::fetchProgress,
            this,
            [this](const CollabContentHash& hash, qint64 received, qint64 total) {
                if (decksForHash(hash).isEmpty()) {
                    return;
                }
                emit transferProgress(hash, received, total);
            });
    connect(m_pTransfer,
            &CollabTransferService::fetchFinished,
            this,
            [this](const CollabContentHash& hash, const QString& path) {
                if (m_analysisFetching.contains(hash.bytes)) {
                    // The analysis bundle for `hash`'s audio arrived.
                    const CollabContentHash audioHash =
                            m_analysisFetching.take(hash.bytes);
                    CollabTrackAnalysis analysis;
                    QFile blobFile(path);
                    QByteArray blob;
                    if (blobFile.open(QIODevice::ReadOnly)) {
                        blob = blobFile.readAll();
                    }
                    if (!decodeCollabTrackAnalysisBundle(blob, &analysis)) {
                        failIncoming(audioHash,
                                tr("The analysis data is invalid."));
                        return;
                    }
                    m_analysisReady.insert(audioHash.bytes, analysis);
                    maybeEmitLoadRequested(audioHash);
                    return;
                }
                if (decksForHash(hash).isEmpty() && !m_audioReady.contains(hash.bytes)) {
                    return;
                }
                m_audioReady.insert(hash.bytes, path);
                if (m_pHashIndex != nullptr) {
                    m_pHashIndex->insert(path, hash);
                }
                maybeEmitLoadRequested(hash);
            });
    connect(m_pTransfer,
            &CollabTransferService::fetchFailed,
            this,
            [this](const CollabContentHash& hash, const QString& error) {
                if (m_analysisFetching.contains(hash.bytes)) {
                    const CollabContentHash audioHash =
                            m_analysisFetching.take(hash.bytes);
                    failIncoming(audioHash,
                            tr("The analysis data transfer failed: %1").arg(error));
                    return;
                }
                failIncoming(hash, error);
            });
}

bool CollabTrackShare::requestLoad(
        int deckId,
        const QString& path,
        const QByteArray& analysisBlob,
        QString* pError) {
    if (!isValidCollabDeckId(deckId)) {
        setError(pError, tr("Deck must be between 1 and 4."));
        return false;
    }
    if (m_pSession == nullptr ||
            m_pSession->state() != CollabSession::State::Connected) {
        setError(pError, tr("Connect both peers first."));
        return false;
    }
    if (m_pTransfer == nullptr || !m_pTransfer->isListening()) {
        setError(pError, tr("The track transfer server is not running."));
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        setError(pError, tr("The track file does not exist."));
        return false;
    }

    CollabContentHash hash;
    const std::optional<CollabContentHash> cached =
            m_pHashIndex != nullptr ? m_pHashIndex->lookup(path) : std::nullopt;
    if (cached.has_value()) {
        hash = *cached;
    } else if (!collabHashFile(path, &hash, pError)) {
        return false;
    }
    if (m_pHashIndex != nullptr) {
        m_pHashIndex->insert(path, hash);
    }
    m_pTransfer->serveFile(hash, path);

    CollabLoadRequest request;
    request.deckId = static_cast<uint16_t>(deckId);
    request.track.hash = hash;
    request.track.size = static_cast<uint64_t>(info.size());
    request.track.suffix = info.suffix();
    request.track.title = info.fileName();
    request.track.transferPort = m_pTransfer->localPort();
    // The peer pulls the analysis bundle after the audio, so both decks see
    // the same waveform and beat grid without re-analyzing. The bundle is
    // content-addressed like the audio: the request carries its SHA-256.
    request.requireAnalysis = true;
    if (!analysisBlob.isEmpty()) {
        CollabContentHash analysisHash;
        if (collabHashBytes(analysisBlob, &analysisHash)) {
            QString error;
            if (serveAnalysis(analysisHash, analysisBlob, &error)) {
                request.analysisHash = analysisHash;
            } else {
                kLogger.warning() << "Collab: cannot serve the analysis bundle:"
                                  << error;
            }
        }
    }

    if (!m_pSession->sendMessage(CollabMessageType::LoadRequest,
                encodeCollabLoadRequest(request),
                CollabFlagReliable)) {
        setError(pError, tr("Cannot send the load request to the peer."));
        return false;
    }
    m_pendingOut.insert(deckId, hash);
    return true;
}

bool CollabTrackShare::handlePacket(const CollabPacket& packet) {
    switch (packet.header.type) {
    case CollabMessageType::LoadRequest: {
        CollabLoadRequest request;
        if (!decodeCollabLoadRequest(packet.payload, &request)) {
            return true;
        }
        onLoadRequest(request);
        return true;
    }
    case CollabMessageType::TrackNeed: {
        // The peer will pull the file from our transfer server, which already
        // serves it because `requestLoad` registered it.
        CollabTrackNeed need;
        decodeCollabTrackNeed(packet.payload, &need);
        return true;
    }
    case CollabMessageType::LoadReady:
    case CollabMessageType::LoadFailed: {
        CollabLoadReply reply;
        if (!decodeCollabLoadReply(packet.payload, &reply)) {
            return true;
        }
        onLoadReply(reply);
        return true;
    }
    default:
        return false;
    }
}

void CollabTrackShare::reportLoadResult(int deckId,
        const CollabContentHash& hash,
        bool ok,
        const QString& reason) {
    const auto it = m_pendingIn.constFind(deckId);
    if (it == m_pendingIn.constEnd() || it.value() != hash) {
        return;
    }
    m_pendingIn.remove(deckId);
    sendReply(deckId, hash, ok, reason);
    if (ok) {
        emit loadCompleted(deckId, hash);
    } else {
        emit loadFailed(deckId,
                hash,
                reason.isEmpty() ? tr("The track could not be loaded.") : reason);
    }
}

bool CollabTrackShare::hasPendingLoad(int deckId) const {
    return m_pendingOut.contains(deckId) || m_pendingIn.contains(deckId);
}

bool CollabTrackShare::hasOutgoingLoad(int deckId) const {
    return m_pendingOut.contains(deckId);
}

bool CollabTrackShare::hasIncomingLoad(int deckId) const {
    return m_pendingIn.contains(deckId);
}

void CollabTrackShare::clear() {
    m_pendingOut.clear();
    m_pendingIn.clear();
    m_incomingEndpoints.clear();
    m_audioReady.clear();
    m_analysisReady.clear();
    for (auto it = m_analysisFetching.constBegin();
            it != m_analysisFetching.constEnd();
            ++it) {
        CollabContentHash key;
        key.bytes = it.key();
        m_pTransfer->cancelFetch(key);
    }
    m_analysisFetching.clear();
}

void CollabTrackShare::onLoadRequest(const CollabLoadRequest& request) {
    const int deckId = request.deckId;
    const auto pending = m_pendingIn.constFind(deckId);
    if (pending != m_pendingIn.constEnd() &&
            pending.value() == request.track.hash) {
        // Already preparing this track for this deck.
        return;
    }
    m_pendingIn.insert(deckId, request.track.hash);

    const std::optional<QString> local = findLocalPath(request.track.hash);
    if (local.has_value()) {
        // Both peers already have the audio; no transfer and no analysis
        // bundle are needed.
        emit loadRequested(deckId, request.track.hash, *local);
        return;
    }

    if (request.track.transferPort == 0) {
        const QString error = tr("The peer did not announce a transfer port.");
        failIncoming(request.track.hash, error);
        return;
    }
    const QHostAddress peer(m_pSession->remoteAddress());
    if (peer.isNull()) {
        const QString error = tr("The peer address is not available.");
        failIncoming(request.track.hash, error);
        return;
    }
    m_incomingEndpoints.insert(request.track.hash.bytes,
            IncomingEndpoint{peer, request.track.transferPort, request.analysisHash});

    QString error;
    if (!m_pTransfer->fetch(peer,
                request.track.transferPort,
                request.track.hash,
                request.track.suffix,
                &error)) {
        failIncoming(request.track.hash, error);
        return;
    }

    CollabTrackNeed need;
    need.hash = request.track.hash;
    m_pSession->sendMessage(CollabMessageType::TrackNeed,
            encodeCollabTrackNeed(need),
            CollabFlagReliable);
    emit transferProgress(request.track.hash, 0, request.track.size);
}

void CollabTrackShare::onLoadReply(const CollabLoadReply& reply) {
    const auto it = m_pendingOut.constFind(reply.deckId);
    if (it == m_pendingOut.constEnd() || it.value() != reply.hash) {
        return;
    }
    m_pendingOut.remove(reply.deckId);
    if (reply.status == CollabLoadStatus::Ready) {
        emit loadCompleted(reply.deckId, reply.hash);
        return;
    }
    emit loadFailed(reply.deckId,
            reply.hash,
            reply.reason.isEmpty()
                    ? tr("The peer could not load the track.")
                    : reply.reason);
}

void CollabTrackShare::sendReply(int deckId,
        const CollabContentHash& hash,
        bool ok,
        const QString& reason) {
    if (m_pSession == nullptr) {
        return;
    }
    CollabLoadReply reply;
    reply.deckId = static_cast<uint16_t>(deckId);
    reply.hash = hash;
    reply.status = ok ? CollabLoadStatus::Ready : CollabLoadStatus::Failed;
    reply.reasonCode = ok ? 0
                          : (reason.isEmpty() ? kReasonLocalLoadFailed
                                              : kReasonTransferFailed);
    reply.reason = reason;
    m_pSession->sendMessage(
            ok ? CollabMessageType::LoadReady : CollabMessageType::LoadFailed,
            encodeCollabLoadReply(reply),
            CollabFlagReliable);
}

bool CollabTrackShare::serveAnalysis(const CollabContentHash& analysisHash,
        const QByteArray& blob,
        QString* pError) {
    if (m_pCache == nullptr || m_pTransfer == nullptr) {
        setError(pError, tr("Track sharing is not configured."));
        return false;
    }
    const QDir analysisDir(QDir(m_pCache->root()).filePath(QStringLiteral("analysis")));
    if (!analysisDir.mkpath(QStringLiteral("."))) {
        setError(pError, tr("Cannot create the analysis directory."));
        return false;
    }
    const QString path = analysisDir.filePath(
            analysisHash.toHex() + QStringLiteral(".bin"));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(blob) != blob.size() || !file.commit()) {
        setError(pError, tr("Cannot write the analysis bundle."));
        return false;
    }
    m_pTransfer->serveFile(analysisHash, path);
    return true;
}

void CollabTrackShare::startAnalysisFetch(const CollabContentHash& audioHash,
        const CollabContentHash& analysisHash) {
    const auto endpoint = m_incomingEndpoints.constFind(audioHash.bytes);
    if (endpoint == m_incomingEndpoints.constEnd()) {
        // No endpoint (for example a resumed local state); skip analysis so
        // the load still completes and local analysis fills the gap.
        return;
    }
    if (m_analysisReady.contains(audioHash.bytes) ||
            m_analysisFetching.contains(analysisHash.bytes)) {
        return;
    }
    QString error;
    if (!m_pTransfer->fetch(endpoint->peer,
                endpoint->port,
                analysisHash,
                QStringLiteral("bin"),
                &error)) {
        kLogger.warning() << "Collab: analysis transfer failed:" << error;
        return;
    }
    m_analysisFetching.insert(analysisHash.bytes, audioHash);
}

void CollabTrackShare::maybeEmitLoadRequested(
        const CollabContentHash& audioHash) {
    if (!m_audioReady.contains(audioHash.bytes)) {
        return;
    }
    const auto endpoint = m_incomingEndpoints.constFind(audioHash.bytes);
    const CollabContentHash analysisHash =
            endpoint == m_incomingEndpoints.constEnd()
            ? CollabContentHash()
            : endpoint->analysisHash;
    if (!m_analysisReady.contains(audioHash.bytes) &&
            analysisHash.isValid() && !analysisHash.isZero()) {
        startAnalysisFetch(audioHash, analysisHash);
        // The analysis pull is in flight; the load waits for it so the deck
        // gets the peer's waveform and beat grid.
        return;
    }
    const QString path = m_audioReady.take(audioHash.bytes);
    m_incomingEndpoints.remove(audioHash.bytes);
    const QList<int> decks = decksForHash(audioHash);
    for (int deck : decks) {
        emit loadRequested(deck, audioHash, path);
    }
}

void CollabTrackShare::failIncoming(
        const CollabContentHash& hash, const QString& reason) {
    const QList<int> decks = decksForHash(hash);
    for (int deck : decks) {
        m_pendingIn.remove(deck);
        sendReply(deck, hash, false, reason);
        emit loadFailed(deck, hash, reason);
    }
    m_incomingEndpoints.remove(hash.bytes);
    m_audioReady.remove(hash.bytes);
    m_analysisReady.remove(hash.bytes);
    // Cancel an analysis pull that is still in flight for this audio hash.
    for (auto it = m_analysisFetching.begin(); it != m_analysisFetching.end();) {
        if (it.value() == hash) {
            m_pTransfer->cancelFetch(CollabContentHash{it.key()});
            it = m_analysisFetching.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<CollabTrackAnalysis> CollabTrackShare::takeTrackAnalysis(
        const CollabContentHash& hash) {
    const auto it = m_analysisReady.constFind(hash.bytes);
    if (it == m_analysisReady.constEnd()) {
        return std::nullopt;
    }
    const CollabTrackAnalysis analysis = it.value();
    m_analysisReady.erase(it);
    return analysis;
}

std::optional<QString> CollabTrackShare::findLocalPath(
        const CollabContentHash& hash) const {
    if (m_pCache != nullptr) {
        const std::optional<QString> cached = m_pCache->find(hash);
        if (cached.has_value()) {
            return cached;
        }
    }
    if (m_pHashIndex != nullptr) {
        return m_pHashIndex->findPath(hash);
    }
    return std::nullopt;
}

QList<int> CollabTrackShare::decksForHash(const CollabContentHash& hash) const {
    QList<int> decks;
    for (auto it = m_pendingIn.constBegin(); it != m_pendingIn.constEnd(); ++it) {
        if (it.value() == hash) {
            decks.append(it.key());
        }
    }
    return decks;
}

} // namespace collab
} // namespace mixxx
