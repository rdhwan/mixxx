#include "collab/collabtrackshare.h"

#include <QFileInfo>
#include <QHostAddress>

#include "collab/collabsession.h"
#include "collab/tracktransfer.h"
#include "moc_collabtrackshare.cpp"

namespace mixxx {
namespace collab {

namespace {

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
                if (m_pHashIndex != nullptr) {
                    m_pHashIndex->insert(path, hash);
                }
                const QList<int> decks = decksForHash(hash);
                for (int deck : decks) {
                    emit loadRequested(deck, hash, path);
                }
            });
    connect(m_pTransfer,
            &CollabTransferService::fetchFailed,
            this,
            [this](const CollabContentHash& hash, const QString& error) {
                const QList<int> decks = decksForHash(hash);
                for (int deck : decks) {
                    m_pendingIn.remove(deck);
                    sendReply(deck, hash, false, error);
                    emit loadFailed(deck, hash, error);
                }
            });
}

bool CollabTrackShare::requestLoad(
        int deckId, const QString& path, QString* pError) {
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
        emit loadRequested(deckId, request.track.hash, *local);
        return;
    }

    if (request.track.transferPort == 0) {
        const QString error = tr("The peer did not announce a transfer port.");
        m_pendingIn.remove(deckId);
        sendReply(deckId, request.track.hash, false, error);
        emit loadFailed(deckId, request.track.hash, error);
        return;
    }
    const QHostAddress peer(m_pSession->remoteAddress());
    if (peer.isNull()) {
        const QString error = tr("The peer address is not available.");
        m_pendingIn.remove(deckId);
        sendReply(deckId, request.track.hash, false, error);
        emit loadFailed(deckId, request.track.hash, error);
        return;
    }

    QString error;
    if (!m_pTransfer->fetch(peer,
                request.track.transferPort,
                request.track.hash,
                request.track.suffix,
                &error)) {
        m_pendingIn.remove(deckId);
        sendReply(deckId, request.track.hash, false, error);
        emit loadFailed(deckId, request.track.hash, error);
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
