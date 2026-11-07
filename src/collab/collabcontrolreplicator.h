#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "collab/collabprotocol.h"
#include "collab/controlmap.h"
#include "collab/deckmap.h"

class QTimer;

class ControlProxy;

namespace mixxx {
namespace collab {

class SteadyCollabClock;

/// Replicates a negotiated set of controls between two peers.
///
/// Local changes are coalesced into deltas at a bounded rate. Remote values are
/// applied through the same `ControlProxy` used to observe local changes, so
/// `ControlProxy` suppresses the echo. Engine confirmations, which arrive from
/// a different setter, are suppressed by a short value/time guard.
///
/// The class does not own the transport: `start` takes a send callback, which
/// keeps it testable without sockets.
class CollabControlReplicator : public QObject {
    Q_OBJECT
  public:
    /// `type` is ControlDelta or ControlKeyframe.
    using SendFunction = std::function<bool(
            CollabMessageType type, const QByteArray& payload)>;

    explicit CollabControlReplicator(QObject* pParent = nullptr);
    ~CollabControlReplicator() override;

    /// Binds every control in `map` and starts replicating. Returns false and
    /// sets `errorString()` when no local control could be bound.
    ///
    /// With `enforceOwnership`, local changes to a deck owned by the other peer
    /// (and to the crossfader on the guest) are reverted and reported through
    /// `localInputBlocked`.
    bool start(const CollabControlMap& map,
            const CollabDeckMap& deckMap,
            SendFunction send,
            bool enforceOwnership = true);
    void stop();
    bool isRunning() const {
        return m_pClock != nullptr;
    }

    /// Sends every mapped value. Used on join, on reconnect and every five
    /// seconds. The keyframe epoch is fixed until the replicator restarts.
    bool sendKeyframe();

    /// Sends dirty values now instead of waiting for the next flush tick.
    void flush();

    /// Applies a ControlDelta or ControlKeyframe packet. Returns true when the
    /// packet was consumed.
    bool handlePacket(const CollabPacket& packet);

    void setKeyframeIntervalMs(int intervalMs);
    /// Minimum difference that counts as a change.
    void setDeadband(double deadband);
    /// Window after a remote application in which a matching local change is
    /// treated as an engine confirmation.
    void setEchoGuardMs(int guardMs);

    QString errorString() const {
        return m_errorString;
    }
    uint32_t localKeyframeEpoch() const {
        return m_localEpoch;
    }

  signals:
    void keyframeApplied(uint32_t epoch);
    /// A packet was structurally valid but referred to an unknown control or a
    /// stale sequence.
    void packetRejected(const QString& reason);
    /// Local input was ignored because the control belongs to the other peer.
    /// `sessionDeck` is zero for the crossfader.
    void localInputBlocked(int sessionDeck, const QString& item);

  private:
    struct Entry {
        CollabControlId id;
        ConfigKey sessionKey;
        ControlProxy* pProxy;
        double lastSentValue;
        double lastRemoteValue;
        qint64 lastRemoteUs;
        bool hasRemoteValue;
        bool dirty;
        bool localInputAllowed;
        int sessionDeck;
    };

    void onLocalChange(int index, double value);
    void revertBlockedInput(Entry* pEntry);
    void applyValue(Entry* pEntry, double value);
    /// Applies a packet's values in an order the engine controls depend on:
    /// a loop start before its end, and loop_enabled after both.
    void applyValues(const QList<CollabControlValue>& values);
    int applyPriority(CollabControlId id);
    void sendDelta();
    void sendKeyframeNow();
    Entry* entryForId(CollabControlId id);

    std::unique_ptr<SteadyCollabClock> m_pClock;
    QTimer* m_pFlushTimer;
    QTimer* m_pKeyframeTimer;
    SendFunction m_send;
    std::vector<Entry> m_entries;
    uint32_t m_localEpoch = 1;
    uint32_t m_localDeltaSeq = 0;
    uint32_t m_remoteEpoch = 1;
    uint32_t m_lastRemoteDeltaSeq = 0;
    bool m_remoteHasKeyframe = false;
    /// True while this replicator is writing controls, either with a remote
    /// value or with a value restored by the ownership lock. Mixxx controls
    /// are connected to each other (for example the loop start/end pair), so a
    /// single write can synchronously change another control and call
    /// onLocalChange again. Those nested changes are ours, not local input.
    bool m_applyingValue = false;
    int m_keyframeIntervalMs = 5000;
    double m_deadband = 1e-5;
    int m_echoGuardMs = 250;
    QString m_errorString;
};

} // namespace collab
} // namespace mixxx
