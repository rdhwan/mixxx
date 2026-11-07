#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "collab/clocksync.h"
#include "collab/collabcontrolreplicator.h"
#include "collab/collabdiagnostics.h"
#include "collab/collabsession.h"
#include "collab/collabtrackshare.h"
#include "collab/collabtypes.h"
#include "collab/commandscheduler.h"
#include "collab/deckstatereplicator.h"
#include "collab/trackcache.h"
#include "collab/trackhash.h"

class QTimer;

namespace mixxx {
namespace collab {

class CollabTransferService;
class SteadyCollabClock;

/// Where received tracks and the hash index live. An empty `cacheRoot`
/// disables track sharing.
struct CollabTrackSharingConfig {
    QString cacheRoot;
    QString hashIndexPath;
    /// TCP port for the bulk server. Zero picks an ephemeral port, which is
    /// still reachable because the peer learns it from `LOAD_REQUEST`.
    quint16 transferPort = 0;
};

/// Process-wide owner of the collaboration session.
///
/// A Mixxx process can contain more than one QML engine: the main window and,
/// on legacy skins, the standalone diagnostics window. The session itself must
/// be singular, so this class holds it and every QML proxy instance delegates
/// here. It also owns the clock exchange, the diagnostics log and the command
/// scheduler, keeping all replication state in one place.
class CollabConnection : public QObject {
    Q_OBJECT
  public:
    explicit CollabConnection(QObject* pParent = nullptr);
    ~CollabConnection() override;

    /// The instance shared by every QML engine in this process.
    static CollabConnection* shared();

    /// Hosts a session on `localPort`. `token` authenticates the guest.
    bool host(quint16 localPort,
            const CollabSessionToken& token,
            const QString& diagnosticsPath);

    /// Joins the host at `hostAddress:hostPort`, binding `localPort` locally.
    bool join(const QString& hostAddress,
            quint16 hostPort,
            quint16 localPort,
            const CollabSessionToken& token,
            const QString& diagnosticsPath);

    /// Leaves the session, sends BYE to the peer and stops the timers.
    void leave();

    CollabSession::State state() const;
    /// True while the session is listening, joining or connected.
    bool isActive() const;
    bool isConnected() const;
    bool isHost() const;
    QString errorString() const;
    QString peerAddress() const;
    quint16 localPort() const;

    qint64 nowUs() const;
    /// Host monotonic time on the host; the host clock mapped through the
    /// estimate on the guest.
    qint64 sessionNowUs() const;

    const CollabClockSync& clockSync() const {
        return m_clockSync;
    }
    qint64 offsetUs() const {
        return m_clockSync.offsetUs();
    }
    qint64 rttUs() const {
        return m_clockSync.rttUs();
    }
    qint64 jitterUs() const {
        return m_clockSync.jitterUs();
    }
    double skewPpm() const {
        return m_clockSync.skewPpm();
    }
    int sampleCount() const {
        return m_clockSync.sampleCount();
    }
    bool hasClockEstimate() const {
        return m_clockSync.hasEstimate();
    }
    /// Milliseconds since the last clock reply, or -1.
    int lastResponseAgeMs() const;
    void clearStatistics();

    /// Largest playhead correction applied to a remote deck, in milliseconds.
    /// Zero on the owner's own decks.
    double lastRemoteDriftMs() const;

    /// Datagram loss estimated from the peer's sequence numbers.
    quint64 receivedPacketCount() const;
    quint64 lostPacketCount() const;
    double packetLossPercent() const;
    /// Scheduled commands that ran since the session started, and those that
    /// were already too late and were dropped.
    quint64 executedCommandCount() const;
    quint64 droppedCommandCount() const;
    /// Largest lateness of an executed command since the session started.
    double maxCommandLatenessMs() const;

    /// Milliseconds between clock exchanges once connected.
    int clockIntervalMs() const {
        return m_clockIntervalMs;
    }
    void setClockIntervalMs(int intervalMs);

    /// Control replication is on by default. Tests that do not build the
    /// engine controls turn it off.
    void setControlReplicationEnabled(bool enabled) {
        m_controlReplicationEnabled = enabled;
    }
    bool isControlReplicationEnabled() const {
        return m_controlReplicationEnabled;
    }

    /// Overrides the replicated control set. Empty means the default set from
    /// `collabDefaultControlKeys()`.
    void setControlKeys(const QList<ConfigKey>& keys) {
        m_controlKeys = keys;
    }

    /// Overrides the handshake and liveness timings, mainly for tests and for
    /// tuning a high-latency link.
    void setSessionTimeouts(const CollabSessionTimeouts& timeouts);

    /// True while a lost peer is being retried.
    bool reconnecting() const;
    /// True after a successful connection was lost and before it is restored.
    /// Remote-owned decks keep their last state while stale.
    bool remoteDecksStale() const;

    /// Manual retry: drops the peer association and starts a fresh handshake
    /// without closing the socket.
    bool reconnect();

    /// Schedules a play on `deck` at the next session-time grid boundary that
    /// lies at least `leadMs` ahead, on both peers.
    bool schedulePlay(int deck, int leadMs = 3000);
    void cancelScheduledCommands();

    CollabDiagnostics* diagnostics() {
        return &m_diagnostics;
    }
    bool openDiagnostics(const QString& path);

    /// Configures where received tracks are cached and where the hash index is
    /// stored. An empty cache root disables track sharing. Reconfiguring while
    /// connected restarts the bulk server with the new settings.
    void setTrackSharingConfig(const CollabTrackSharingConfig& config);
    CollabTrackSharingConfig trackSharingConfig() const {
        return m_trackSharingConfig;
    }
    bool isTrackSharingEnabled() const {
        return m_pTrackShare != nullptr;
    }
    /// TCP port the bulk server listens on, or zero when it is not running.
    quint16 trackTransferPort() const;

    /// Hashes a local track, makes it available to the peer and asks the peer
    /// to load it on `deckId`. The deck owner calls this when it loads a track.
    bool requestTrackLoad(
            int deckId, const QString& path, QString* pError = nullptr);
    /// Reports the result of a local load started by `trackLoadRequested`.
    void reportTrackLoadResult(int deckId,
            const CollabContentHash& hash,
            bool ok,
            const QString& reason = QString());
    /// True while `deckId` has an outgoing load request or an incoming load
    /// that has not been reported yet. Playback should stay gated.
    bool hasPendingTrackLoad(int deckId) const;

    /// True when the local user is the owner of `deckId` and may load a
    /// track onto it: the host owns decks 1 and 2, the guest decks 3 and 4.
    bool ownsSessionDeck(int deckId) const;

    /// Last blocked local input, empty when nothing has been blocked. Cleared
    /// when a session starts.
    QString ownershipNotice() const {
        return m_ownershipNotice;
    }

  signals:
    /// The session state or the connected peer changed.
    void stateChanged();
    void statisticsChanged();
    void errorOccurred(const QString& message);
    /// A scheduled play was applied locally. `remote` tells whether the command
    /// came from the peer.
    void playApplied(int deck, double latenessMs, bool remote);
    /// Local input was ignored because that deck belongs to the other peer.
    void localInputBlocked(int sessionDeck, const QString& item);
    /// A keyframe from the peer was applied. The epoch increases on every
    /// reconnect, so a newer value means the peer restarted its stream.
    void controlKeyframeApplied(uint32_t epoch);
    /// The peer was lost and is being retried, or the retry succeeded.
    void reconnectingChanged(bool reconnecting);
    /// The peer asked us to load a track on a deck. Load it, then call
    /// `reportTrackLoadResult`.
    void trackLoadRequested(int deckId,
            const mixxx::collab::CollabContentHash& hash,
            const QString& path);
    /// The peer confirmed that it loaded a track we asked for, or a local load
    /// we reported succeeded.
    void trackLoadCompleted(
            int deckId, const mixxx::collab::CollabContentHash& hash);
    void trackLoadFailed(int deckId,
            const mixxx::collab::CollabContentHash& hash,
            const QString& reason);
    void trackTransferProgress(const mixxx::collab::CollabContentHash& hash,
            qint64 received,
            qint64 total);
    /// Track sharing was (re)configured; `trackTransferPort` may have changed.
    void trackSharingChanged();

  private:
    void onStateChanged(CollabSession::State state);
    void onPacketReceived(const CollabPacket& packet);
    void onClockPing(const CollabPacket& packet);
    void onClockPong(const CollabPacket& packet);
    void onCommand(const CollabPacket& packet);
    void startControlReplication();
    void stopControlReplication();
    void startDeckStateReplication();
    void stopDeckStateReplication();
    bool startTrackSharing();
    void stopTrackSharing();
    void saveTrackHashIndex();
    void sendClockPing();
    void tickScheduler();
    bool scheduleLocalPlay(
            int deck, qint64 targetSessionUs, quint64 commandId, bool remote);
    quint64 makeCommandId();

    std::unique_ptr<SteadyCollabClock> m_pClock;
    CollabSession* m_pSession;
    CollabClockSync m_clockSync;
    CollabDiagnostics m_diagnostics;
    CollabCommandScheduler m_scheduler;
    CollabControlReplicator m_controlReplicator;
    DeckStateReplicator m_deckStateReplicator;
    CollabTrackSharingConfig m_trackSharingConfig;
    std::unique_ptr<CollabTrackCache> m_pTrackCache;
    CollabTrackHashIndex m_trackHashIndex;
    std::unique_ptr<CollabTransferService> m_pTransferService;
    std::unique_ptr<CollabTrackShare> m_pTrackShare;
    QTimer* m_pClockTimer;
    QTimer* m_pSchedulerTimer;
    int m_clockIntervalMs = 200;
    qint64 m_lastPongUs = 0;
    QString m_ownershipNotice;
    quint64 m_nextCommandId = 1;
    bool m_controlReplicationEnabled = true;
    bool m_hasBeenConnected = false;
    uint32_t m_lastPeerKeyframeEpoch = 0;
    QList<ConfigKey> m_controlKeys;
    QString m_errorString;
};

} // namespace collab
} // namespace mixxx
