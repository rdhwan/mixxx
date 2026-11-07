#include "collab/deckstatereplicator.h"

#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>

#include "collab/collabclock.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "moc_deckstatereplicator.cpp"

namespace mixxx {
namespace collab {

namespace {

/// Minimum spacing between anchors while the owner is moving the playhead.
/// Fast enough to follow a scratch, slow enough not to flood the link.
constexpr int kUrgentAnchorMs = 25;

double clampPosition(double value) {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace

DeckStateReplicator::DeckStateReplicator(QObject* pParent)
        : QObject(pParent),
          m_pAnchorTimer(new QTimer(this)),
          m_pUrgentTimer(new QTimer(this)) {
    m_pAnchorTimer->setTimerType(Qt::CoarseTimer);
    connect(m_pAnchorTimer,
            &QTimer::timeout,
            this,
            &DeckStateReplicator::onAnchorTimer);

    m_pUrgentTimer->setSingleShot(true);
    connect(m_pUrgentTimer,
            &QTimer::timeout,
            this,
            &DeckStateReplicator::flushPendingAnchors);
}

DeckStateReplicator::~DeckStateReplicator() {
    stop();
}

bool DeckStateReplicator::start(const CollabDeckMap& deckMap,
        SendFunction send,
        SessionNowFunction sessionNowUs,
        ClockReadyFunction clockReady) {
    stop();
    m_errorString.clear();
    m_send = std::move(send);
    m_sessionNowUs = std::move(sessionNowUs);
    m_clockReady = std::move(clockReady);
    if (!m_send || !m_sessionNowUs) {
        m_errorString = tr("Deck replication needs a send function and a session clock");
        return false;
    }

    m_decks.assign(kCollabSessionDeckCount, Deck{});
    int availableDecks = 0;
    for (int sessionDeck = 1; sessionDeck <= kCollabSessionDeckCount;
            ++sessionDeck) {
        Deck& deck = m_decks[static_cast<std::size_t>(sessionDeck - 1)];
        deck.sessionDeck = sessionDeck;
        deck.owned = deckMap.ownsSessionDeck(sessionDeck);
        const QString group = deckMap.localGroupForSessionDeck(sessionDeck);
        if (group.isEmpty()) {
            continue;
        }

        const std::array<QString, 9> items = {
                QStringLiteral("play"),
                QStringLiteral("playposition"),
                QStringLiteral("duration"),
                QStringLiteral("track_samples"),
                QStringLiteral("rate_ratio"),
                QStringLiteral("reverse"),
                QStringLiteral("loop_enabled"),
                QStringLiteral("loop_start_position"),
                QStringLiteral("loop_end_position"),
        };
        bool complete = true;
        for (const QString& item : items) {
            if (!ControlObject::exists(ConfigKey(group, item))) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            continue;
        }

        deck.pPlay = new ControlProxy(ConfigKey(group, QStringLiteral("play")), this);
        deck.pPlayPosition = new ControlProxy(
                ConfigKey(group, QStringLiteral("playposition")), this);
        deck.pDuration = new ControlProxy(
                ConfigKey(group, QStringLiteral("duration")), this);
        deck.pTrackSamples = new ControlProxy(
                ConfigKey(group, QStringLiteral("track_samples")), this);
        deck.pRateRatio = new ControlProxy(
                ConfigKey(group, QStringLiteral("rate_ratio")), this);
        deck.pReverse = new ControlProxy(
                ConfigKey(group, QStringLiteral("reverse")), this);
        deck.pLoopEnabled = new ControlProxy(
                ConfigKey(group, QStringLiteral("loop_enabled")), this);
        deck.pLoopStart = new ControlProxy(
                ConfigKey(group, QStringLiteral("loop_start_position")), this);
        deck.pLoopEnd = new ControlProxy(
                ConfigKey(group, QStringLiteral("loop_end_position")), this);
        deck.available = true;
        ++availableDecks;

        if (!deck.owned) {
            continue;
        }

        const int index = sessionDeck - 1;
        deck.pPlay->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pRateRatio->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pReverse->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pLoopEnabled->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pLoopStart->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pLoopEnd->connectValueChanged(
                this, [this, index](double) { onOwnerControlChanged(index); });
        deck.pPlayPosition->connectValueChanged(
                this, [this, index](double) { onOwnerPositionChanged(index); });
        deck.lastPosition = deck.pPlayPosition->get();
        deck.lastPositionUs = m_sessionNowUs();
    }

    if (availableDecks == 0) {
        m_errorString = tr("None of the session decks exist locally");
        stop();
        return false;
    }

    m_running = true;
    m_pAnchorTimer->start(m_anchorIntervalMs);
    return true;
}

void DeckStateReplicator::stop() {
    m_running = false;
    if (m_pAnchorTimer != nullptr) {
        m_pAnchorTimer->stop();
    }
    if (m_pUrgentTimer != nullptr) {
        m_pUrgentTimer->stop();
    }
    for (Deck& deck : m_decks) {
        delete deck.pPlay;
        delete deck.pPlayPosition;
        delete deck.pDuration;
        delete deck.pTrackSamples;
        delete deck.pRateRatio;
        delete deck.pReverse;
        delete deck.pLoopEnabled;
        delete deck.pLoopStart;
        delete deck.pLoopEnd;
    }
    m_decks.clear();
    m_send = nullptr;
    m_sessionNowUs = nullptr;
    m_clockReady = nullptr;
}

void DeckStateReplicator::setAnchorIntervalMs(int intervalMs) {
    m_anchorIntervalMs = std::max(intervalMs, 200);
    if (m_pAnchorTimer->isActive()) {
        m_pAnchorTimer->start(m_anchorIntervalMs);
    }
}

void DeckStateReplicator::setSeekThresholdMs(double thresholdMs) {
    m_seekThresholdMs = std::max(thresholdMs, 0.0);
}

void DeckStateReplicator::setScratchThresholdMs(double thresholdMs) {
    m_scratchThresholdMs = std::max(thresholdMs, 1.0);
}

double DeckStateReplicator::lastCorrectionMs(int sessionDeckId) const {
    const Deck* pDeck = deckForSessionDeck(sessionDeckId);
    return pDeck != nullptr ? pDeck->lastCorrectionMs : 0.0;
}

DeckStateReplicator::Deck* DeckStateReplicator::deckForSessionDeck(
        int sessionDeckId) {
    if (!isValidCollabDeckId(sessionDeckId) || m_decks.empty()) {
        return nullptr;
    }
    Deck& deck = m_decks[static_cast<std::size_t>(sessionDeckId - 1)];
    return deck.available ? &deck : nullptr;
}

const DeckStateReplicator::Deck* DeckStateReplicator::deckForSessionDeck(
        int sessionDeckId) const {
    if (!isValidCollabDeckId(sessionDeckId) || m_decks.empty()) {
        return nullptr;
    }
    const Deck& deck = m_decks[static_cast<std::size_t>(sessionDeckId - 1)];
    return deck.available ? &deck : nullptr;
}

bool DeckStateReplicator::clockReady() const {
    return !m_clockReady || m_clockReady();
}

double DeckStateReplicator::effectiveRate(const Deck& deck) {
    const double rate = deck.pRateRatio->get();
    return deck.pReverse->get() > 0.5 ? -rate : rate;
}

void DeckStateReplicator::onAnchorTimer() {
    if (!m_running) {
        return;
    }
    const qint64 nowUs = m_sessionNowUs();
    for (Deck& deck : m_decks) {
        if (!deck.available || !deck.owned) {
            continue;
        }
        if (deck.pendingAnchor &&
                nowUs - deck.lastSentUs >= kUrgentAnchorMs * 1000LL) {
            sendAnchor(deck.sessionDeck);
            continue;
        }
        if (deck.pPlay->get() > 0.5) {
            sendAnchor(deck.sessionDeck);
        }
    }
}

void DeckStateReplicator::onOwnerControlChanged(int index) {
    if (!m_running || index < 0 || index >= static_cast<int>(m_decks.size())) {
        return;
    }
    const Deck& deck = m_decks[static_cast<std::size_t>(index)];
    if (deck.owned) {
        sendAnchor(deck.sessionDeck);
    }
}

void DeckStateReplicator::onOwnerPositionChanged(int index) {
    if (!m_running || index < 0 || index >= static_cast<int>(m_decks.size())) {
        return;
    }
    Deck& deck = m_decks[static_cast<std::size_t>(index)];
    if (!deck.owned) {
        return;
    }

    const double position = deck.pPlayPosition->get();
    const double duration = deck.pDuration->get();
    const qint64 nowUs = m_sessionNowUs();
    const double elapsedSeconds =
            std::max<qint64>(0, nowUs - deck.lastPositionUs) / 1e6;
    double expected = deck.lastPosition;
    if (deck.pPlay->get() > 0.5 && duration > 0.0) {
        expected += effectiveRate(deck) * elapsedSeconds / duration;
    }

    deck.lastPosition = position;
    deck.lastPositionUs = nowUs;

    if (!(duration > 0.0)) {
        return;
    }

    // Scratches and slow nudges arrive as many small per-buffer increments.
    // Compare the sum of the deviations instead of one update, otherwise no
    // single step crosses the threshold and the follower never moves.
    deck.positionResidual += (position - expected) * duration * 1000.0;
    if (std::abs(deck.positionResidual) < m_scratchThresholdMs) {
        return;
    }
    if (nowUs - deck.lastSentUs >= kUrgentAnchorMs * 1000LL) {
        sendAnchor(deck.sessionDeck);
        return;
    }
    deck.pendingAnchor = true;
    if (!m_pUrgentTimer->isActive()) {
        m_pUrgentTimer->start(kUrgentAnchorMs);
    }
}

void DeckStateReplicator::flushPendingAnchors() {
    if (!m_running) {
        return;
    }
    const qint64 nowUs = m_sessionNowUs();
    bool stillPending = false;
    for (Deck& deck : m_decks) {
        if (!deck.available || !deck.owned || !deck.pendingAnchor) {
            continue;
        }
        if (nowUs - deck.lastSentUs >= kUrgentAnchorMs * 1000LL) {
            sendAnchor(deck.sessionDeck);
        } else {
            stillPending = true;
        }
    }
    if (stillPending) {
        m_pUrgentTimer->start(kUrgentAnchorMs);
    }
}

bool DeckStateReplicator::sendAnchor(int sessionDeckId) {
    Deck* pDeck = deckForSessionDeck(sessionDeckId);
    if (!m_running || pDeck == nullptr || !pDeck->owned || !m_send ||
            !clockReady()) {
        return false;
    }

    const double duration = pDeck->pDuration->get();
    if (!(duration > 0.0)) {
        return false;
    }

    CollabDeckAnchor anchor;
    anchor.deckId = static_cast<uint16_t>(sessionDeckId);
    anchor.stateVersion = pDeck->nextStateVersion++;
    anchor.durationSeconds = duration;
    anchor.position = clampPosition(pDeck->pPlayPosition->get());
    anchor.rate = effectiveRate(*pDeck);
    anchor.playing = pDeck->pPlay->get() > 0.5;
    anchor.loopEnabled = pDeck->pLoopEnabled->get() > 0.5;

    const double trackSamples = pDeck->pTrackSamples->get();
    if (anchor.loopEnabled && trackSamples > 0.0) {
        const double loopStart = pDeck->pLoopStart->get() / trackSamples;
        const double loopEnd = pDeck->pLoopEnd->get() / trackSamples;
        if (loopStart >= 0.0 && loopEnd > loopStart && loopEnd <= 1.0) {
            anchor.loopStart = loopStart;
            anchor.loopEnd = loopEnd;
        } else {
            anchor.loopEnabled = false;
        }
    } else {
        anchor.loopEnabled = false;
    }

    const qint64 nowUs = m_sessionNowUs();
    anchor.anchorSessionTimeUs = static_cast<uint64_t>(nowUs);
    pDeck->lastSentUs = nowUs;
    pDeck->pendingAnchor = false;
    if (!m_send(CollabMessageType::DeckAnchor,
                encodeCollabDeckAnchor(anchor))) {
        return false;
    }
    // The anchor carries the position it was read at, so whatever deviation
    // had accumulated is now covered.
    pDeck->positionResidual = 0.0;
    ++m_anchorCount;
    return true;
}

bool DeckStateReplicator::handlePacket(const CollabPacket& packet) {
    if (!m_running) {
        return false;
    }
    if (packet.header.type != CollabMessageType::DeckAnchor) {
        return false;
    }

    CollabDeckAnchor anchor;
    if (!decodeCollabDeckAnchor(packet.payload, &anchor)) {
        emit packetRejected(tr("Malformed deck anchor"));
        return true;
    }

    Deck* pDeck = deckForSessionDeck(anchor.deckId);
    if (pDeck == nullptr || pDeck->owned || !clockReady()) {
        return true;
    }
    if (anchor.stateVersion <= pDeck->lastRemoteVersion) {
        return true;
    }
    pDeck->lastRemoteVersion = anchor.stateVersion;
    applyAnchor(anchor);
    return true;
}

void DeckStateReplicator::applyAnchor(const CollabDeckAnchor& anchor) {
    Deck* pDeck = deckForSessionDeck(anchor.deckId);
    if (pDeck == nullptr || !(anchor.durationSeconds > 0.0)) {
        return;
    }

    // A different track makes the normalized position meaningless. M1 assumes
    // both peers have the same file loaded.
    const double localDuration = pDeck->pDuration->get();
    if (!(localDuration > 0.0) ||
            std::abs(localDuration - anchor.durationSeconds) >
                    std::max(0.5, anchor.durationSeconds * 0.01)) {
        return;
    }

    const double localPosition = pDeck->pPlayPosition->get();
    if (localPosition < 0.0 || localPosition > 1.0) {
        return;
    }

    double expected = anchor.position;
    if (anchor.playing) {
        const qint64 nowUs = m_sessionNowUs();
        const qint64 elapsedUs = std::max<qint64>(
                0, nowUs - static_cast<qint64>(anchor.anchorSessionTimeUs));
        expected += anchor.rate * (elapsedUs / 1e6) /
                anchor.durationSeconds;
    }
    if (anchor.loopEnabled && anchor.loopEnd > anchor.loopStart) {
        const double loopLength = anchor.loopEnd - anchor.loopStart;
        double wrapped = std::fmod(expected - anchor.loopStart, loopLength);
        if (wrapped < 0.0) {
            wrapped += loopLength;
        }
        expected = anchor.loopStart + wrapped;
    }
    expected = clampPosition(expected);

    const double errorMs =
            (expected - localPosition) * anchor.durationSeconds * 1000.0;
    pDeck->lastCorrectionMs = errorMs;
    if (std::abs(errorMs) < m_seekThresholdMs) {
        return;
    }

    pDeck->pPlayPosition->set(expected);
    emit correctionApplied(anchor.deckId, errorMs);
}

} // namespace collab
} // namespace mixxx
