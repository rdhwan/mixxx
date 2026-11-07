#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <vector>

#include "collab/collabprotocol.h"
#include "collab/deckmap.h"

class QTimer;

class ControlProxy;

namespace mixxx {
namespace collab {

/// Replicates the owner's playhead with periodic anchors.
///
/// The owner sends a normalized playhead position and the session time it was
/// read at. The follower computes where the owner's playhead should be now and
/// corrects the local deck with a seek. Positions are normalized to the track
/// duration so the two peers may run their engines at different sample rates.
///
/// The class is deliberately independent of the network: the caller provides
/// the send function, the session clock and a readiness predicate, which makes
/// it testable without sockets.
class DeckStateReplicator : public QObject {
    Q_OBJECT
  public:
    using SendFunction = std::function<bool(
            CollabMessageType type, const QByteArray& payload)>;
    using SessionNowFunction = std::function<qint64()>;
    using ClockReadyFunction = std::function<bool()>;

    explicit DeckStateReplicator(QObject* pParent = nullptr);
    ~DeckStateReplicator() override;

    /// Starts replicating. `clockReady` may be empty, in which case anchors are
    /// exchanged as soon as the session connects. Returns false when no deck
    /// has the required controls.
    bool start(const CollabDeckMap& deckMap,
            SendFunction send,
            SessionNowFunction sessionNowUs,
            ClockReadyFunction clockReady = {});
    void stop();
    bool isRunning() const {
        return m_running;
    }

    /// Human-readable reason the last start failed.
    QString errorString() const {
        return m_errorString;
    }

    /// Consumes a DeckAnchor packet. Returns true when the packet was handled.
    bool handlePacket(const CollabPacket& packet);

    /// Sends an anchor for an owned deck now. Returns false for a deck this
    /// peer does not own or that has no track.
    bool sendAnchor(int sessionDeckId);

    void setAnchorIntervalMs(int intervalMs);
    /// Seek threshold in milliseconds. Smaller errors are left to the engine.
    void setSeekThresholdMs(double thresholdMs);
    /// A playhead move larger than this within one update is treated as a seek
    /// or a scratch and forwarded immediately.
    void setScratchThresholdMs(double thresholdMs);

    double lastCorrectionMs(int sessionDeckId) const;
    int anchorCount() const {
        return m_anchorCount;
    }

  signals:
    void correctionApplied(int sessionDeckId, double errorMs);
    void packetRejected(const QString& reason);

  private:
    struct Deck {
        int sessionDeck = 0;
        bool available = false;
        bool owned = false;

        ControlProxy* pPlay = nullptr;
        ControlProxy* pPlayPosition = nullptr;
        ControlProxy* pDuration = nullptr;
        ControlProxy* pTrackSamples = nullptr;
        ControlProxy* pRate = nullptr;
        ControlProxy* pLoopEnabled = nullptr;
        ControlProxy* pLoopStart = nullptr;
        ControlProxy* pLoopEnd = nullptr;

        uint32_t nextStateVersion = 1;
        uint32_t lastRemoteVersion = 0;
        double lastPosition = 0.0;
        qint64 lastPositionUs = 0;
        qint64 lastSentUs = 0;
        bool pendingAnchor = false;
        double lastCorrectionMs = 0.0;
    };

    Deck* deckForSessionDeck(int sessionDeckId);
    const Deck* deckForSessionDeck(int sessionDeckId) const;

    void onAnchorTimer();
    void onOwnerControlChanged(int index);
    void onOwnerPositionChanged(int index);
    void flushPendingAnchors();

    void applyAnchor(const CollabDeckAnchor& anchor);
    bool clockReady() const;

    std::vector<Deck> m_decks;
    QTimer* m_pAnchorTimer;
    QTimer* m_pUrgentTimer;
    SendFunction m_send;
    SessionNowFunction m_sessionNowUs;
    ClockReadyFunction m_clockReady;
    int m_anchorIntervalMs = 1000;
    double m_seekThresholdMs = 5.0;
    double m_scratchThresholdMs = 20.0;
    int m_anchorCount = 0;
    bool m_running = false;
    QString m_errorString;
};

} // namespace collab
} // namespace mixxx
