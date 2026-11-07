#pragma once

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <optional>

#include "collab/collabprotocol.h"
#include "collab/trackanalysis.h"
#include "collab/trackcache.h"
#include "collab/trackhash.h"

namespace mixxx {
namespace collab {

class CollabSession;
class CollabTransferService;

/// Coordinates track sharing between the two peers.
///
/// The deck owner has the file. When it loads a track, it calls `requestLoad`,
/// which registers the file with the transfer service and sends a
/// `LOAD_REQUEST` with the content hash and its TCP port. The peer answers with
/// `LOAD_READY` once it has loaded the same audio locally, or with
/// `LOAD_FAILED`.
///
/// If the peer does not have the file, it pulls it from the announced port,
/// verifies the hash, installs it in the cache and then emits `loadRequested`.
/// The application loads the deck and calls `reportLoadResult`; only then is
/// `LOAD_READY` sent back, so the owner can gate playback on it.
class CollabTrackShare : public QObject {
    Q_OBJECT
  public:
    CollabTrackShare(CollabSession* pSession,
            CollabTransferService* pTransfer,
            CollabTrackHashIndex* pHashIndex,
            CollabTrackCache* pCache,
            QObject* pParent = nullptr);

    /// Hashes `path`, makes it available to the peer and asks the peer to load
    /// it on `deckId`. `analysisBlob` is an encoded analysis bundle (see
    /// `trackanalysis.h`) that the peer pulls after the audio so it can skip
    /// re-analysis; empty means "no analysis entries". Returns false when the
    /// session is not connected, the transfer server is not running, or the
    /// file cannot be hashed.
    bool requestLoad(int deckId,
            const QString& path,
            const QByteArray& analysisBlob = QByteArray(),
            QString* pError = nullptr);

    /// Handles track-sharing messages. Returns true when the packet belonged to
    /// this class, whether or not it was valid.
    bool handlePacket(const CollabPacket& packet);

    /// Reports the result of a local load started by `loadRequested`.
    void reportLoadResult(int deckId,
            const CollabContentHash& hash,
            bool ok,
            const QString& reason = QString());

    /// True while `deckId` has an outgoing request or an incoming load that has
    /// not been reported yet.
    bool hasPendingLoad(int deckId) const;
    /// True while `deckId` waits for the peer to load a track we asked for.
    bool hasOutgoingLoad(int deckId) const;
    /// True while `deckId` waits for the local application to load a track the
    /// peer asked for.
    bool hasIncomingLoad(int deckId) const;

    /// Returns and consumes the analysis bundle the peer sent for `hash`, if
    /// it has arrived. The application applies it to the track before
    /// reporting the load result.
    std::optional<CollabTrackAnalysis> takeTrackAnalysis(
            const CollabContentHash& hash);

    /// Drops all pending state. Used when the session ends.
    void clear();

  signals:
    /// The peer asked for this track and it is ready locally. The application
    /// loads the deck and calls `reportLoadResult`.
    void loadRequested(int deckId,
            const mixxx::collab::CollabContentHash& hash,
            const QString& path);
    /// The peer confirmed that it loaded the track (outgoing request), or a
    /// local load we started has been reported (incoming request).
    void loadCompleted(int deckId, const mixxx::collab::CollabContentHash& hash);
    void loadFailed(int deckId,
            const mixxx::collab::CollabContentHash& hash,
            const QString& reason);
    void transferProgress(const mixxx::collab::CollabContentHash& hash,
            qint64 received,
            qint64 total);

  private:
    void onLoadRequest(const CollabLoadRequest& request);
    void onLoadReply(const CollabLoadReply& reply);
    void sendReply(int deckId,
            const CollabContentHash& hash,
            bool ok,
            const QString& reason);
    std::optional<QString> findLocalPath(const CollabContentHash& hash) const;
    QList<int> decksForHash(const CollabContentHash& hash) const;
    /// Serves the analysis bundle under its own content hash so the peer can
    /// pull it like any other file.
    bool serveAnalysis(const CollabContentHash& analysisHash,
            const QByteArray& blob,
            QString* pError);
    /// Starts pulling the peer's analysis bundle for `audioHash`. A failure
    /// only logs: the load then proceeds without analysis instead of getting
    /// stuck, and local analysis fills the gap.
    void startAnalysisFetch(const CollabContentHash& audioHash,
            const CollabContentHash& analysisHash);
    /// Emits `loadRequested` for every deck waiting on `audioHash` once the
    /// audio and the analysis bundle are both ready.
    void maybeEmitLoadRequested(const CollabContentHash& audioHash);
    /// Fails every deck waiting on `audioHash` and drops its state.
    void failIncoming(const CollabContentHash& hash, const QString& reason);

    struct IncomingEndpoint {
        QHostAddress peer;
        quint16 port = 0;
        /// The peer's analysis bundle for this audio; zero when it has none.
        CollabContentHash analysisHash;
    };

    CollabSession* m_pSession;
    CollabTransferService* m_pTransfer;
    CollabTrackHashIndex* m_pHashIndex;
    CollabTrackCache* m_pCache;
    QHash<int, CollabContentHash> m_pendingOut;
    QHash<int, CollabContentHash> m_pendingIn;
    /// Where to pull the analysis bundle from, per audio hash.
    QHash<QByteArray, IncomingEndpoint> m_incomingEndpoints;
    /// Audio file the receiver has fully fetched and verified, per audio hash.
    QHash<QByteArray, QString> m_audioReady;
    /// Analysis bundles that have arrived, per audio hash.
    QHash<QByteArray, CollabTrackAnalysis> m_analysisReady;
    /// Analysis pulls in flight: analysis key -> audio hash it belongs to.
    QHash<QByteArray, CollabContentHash> m_analysisFetching;
};

} // namespace collab
} // namespace mixxx
