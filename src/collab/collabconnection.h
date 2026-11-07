#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "collab/clocksync.h"
#include "collab/collabcontrolreplicator.h"
#include "collab/collabdiagnostics.h"
#include "collab/collabsession.h"
#include "collab/collabtypes.h"
#include "collab/commandscheduler.h"
#include "collab/deckstatereplicator.h"

class QTimer;

namespace mixxx {
namespace collab {

class SteadyCollabClock;

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
