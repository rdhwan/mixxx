#include "collab/collabcontrolreplicator.h"

#include <QTimer>
#include <QtDebug>
#include <algorithm>
#include <cmath>

#include "collab/collabclock.h"
#include "control/controlproxy.h"
#include "moc_collabcontrolreplicator.cpp"

namespace mixxx {
namespace collab {

namespace {

/// Interval of the delta flush. Sixty per second is the protocol limit.
constexpr int kFlushIntervalMs = 16;

/// Values closer than this are considered equal when suppressing an engine
/// confirmation of a remote value.
constexpr double kEchoEpsilon = 1e-9;

/// Controls that act on the rising edge and are released immediately. They
/// must not be coalesced with their own release in one flush window.
bool isMomentaryControl(const QString& item) {
    return item == QStringLiteral("cue_default") ||
            item == QStringLiteral("cue_gotoandstop") ||
            item == QStringLiteral("cue_set") ||
            item == QStringLiteral("cue_clear") ||
            item == QStringLiteral("play_stutter") ||
            (item.startsWith(QStringLiteral("hotcue_")) &&
                    item.endsWith(QStringLiteral("_activate")));
}

} // namespace

CollabControlReplicator::CollabControlReplicator(QObject* pParent)
        : QObject(pParent),
          m_pFlushTimer(new QTimer(this)),
          m_pKeyframeTimer(new QTimer(this)) {
    m_pFlushTimer->setTimerType(Qt::PreciseTimer);
    m_pFlushTimer->setInterval(kFlushIntervalMs);
    connect(m_pFlushTimer, &QTimer::timeout, this, &CollabControlReplicator::flush);

    m_pKeyframeTimer->setTimerType(Qt::CoarseTimer);
    m_pKeyframeTimer->setInterval(m_keyframeIntervalMs);
    connect(m_pKeyframeTimer,
            &QTimer::timeout,
            this,
            &CollabControlReplicator::sendKeyframe);
}

CollabControlReplicator::~CollabControlReplicator() {
    stop();
}

namespace {

/// Returns the session deck of a session-space key, or zero for a key that is
/// not tied to one deck.
int sessionDeckForSessionKey(const ConfigKey& key) {
    const QString& group = key.group;
    if (group.size() != QStringLiteral("[Channel1]").size() ||
            !group.startsWith(QStringLiteral("[Channel"))) {
        return 0;
    }
    bool ok = false;
    const int deck = group.mid(8, 1).toInt(&ok);
    return ok && isValidCollabDeckId(deck) ? deck : 0;
}

} // namespace

bool CollabControlReplicator::start(const CollabControlMap& map,
        const CollabDeckMap& deckMap,
        SendFunction send,
        bool enforceOwnership) {
    stop();
    m_errorString.clear();
    m_send = std::move(send);

    QStringList missing;
    m_entries.reserve(static_cast<std::size_t>(map.size()));
    for (const CollabControlMap::Entry& entry : map.entries()) {
        const ConfigKey localKey = collabLocalControlKey(entry.key, deckMap);
        auto* pProxy = new ControlProxy(localKey, this);
        if (!pProxy->valid()) {
            // Another build or a missing effect chain may not define every
            // control. Replicate the rest instead of failing the session.
            missing.append(localKey.group + QLatin1Char(',') + localKey.item);
            delete pProxy;
            continue;
        }
        delete pProxy;

        Entry replicated;
        replicated.id = entry.id;
        replicated.sessionKey = entry.key;
        replicated.sessionDeck = sessionDeckForSessionKey(entry.key);
        // The crossfader is host-owned; a deck belongs to one peer.
        replicated.localInputAllowed = !enforceOwnership ||
                (replicated.sessionDeck == 0
                                ? entry.key.group != QStringLiteral("[Master]") ||
                                        deckMap.role() == CollabRole::Host
                                : deckMap.ownsSessionDeck(replicated.sessionDeck));
        replicated.pProxy = new ControlProxy(localKey, this);
        replicated.lastSentValue = replicated.pProxy->get();
        replicated.lastRemoteValue = 0.0;
        replicated.lastRemoteUs = 0;
        replicated.hasRemoteValue = false;
        replicated.dirty = false;

        const int index = static_cast<int>(m_entries.size());
        m_entries.push_back(replicated);
        m_entries.back().pProxy->connectValueChanged(
                this, [this, index](double value) {
                    onLocalChange(index, value);
                });
    }

    if (m_entries.empty()) {
        m_errorString = tr("None of the session controls exist locally");
        stop();
        return false;
    }
    if (!missing.isEmpty()) {
        qWarning() << "Collab: controls not available locally:" << missing;
    }

    m_pClock = std::make_unique<SteadyCollabClock>();
    m_localEpoch = 1;
    m_localDeltaSeq = 0;
    m_remoteEpoch = 1;
    m_lastRemoteDeltaSeq = 0;
    m_remoteHasKeyframe = false;
    m_pKeyframeTimer->start(m_keyframeIntervalMs);
    return true;
}

void CollabControlReplicator::stop() {
    if (m_pFlushTimer != nullptr) {
        m_pFlushTimer->stop();
    }
    if (m_pKeyframeTimer != nullptr) {
        m_pKeyframeTimer->stop();
    }
    for (Entry& entry : m_entries) {
        delete entry.pProxy;
    }
    m_entries.clear();
    m_pClock.reset();
    m_send = nullptr;
    m_applyingValue = false;
}

void CollabControlReplicator::setKeyframeIntervalMs(int intervalMs) {
    m_keyframeIntervalMs = std::max(intervalMs, 200);
    if (m_pKeyframeTimer->isActive()) {
        m_pKeyframeTimer->start(m_keyframeIntervalMs);
    }
}

void CollabControlReplicator::setDeadband(double deadband) {
    m_deadband = std::max(deadband, 0.0);
}

void CollabControlReplicator::setEchoGuardMs(int guardMs) {
    m_echoGuardMs = std::max(guardMs, 0);
}

bool CollabControlReplicator::sendKeyframe() {
    if (!isRunning()) {
        return false;
    }
    // The epoch stays fixed for the life of a session, so a lost periodic
    // keyframe does not invalidate the deltas that follow it. A reconnect
    // restarts the replicator and therefore the epoch.
    sendKeyframeNow();
    return true;
}

void CollabControlReplicator::sendKeyframeNow() {
    if (!isRunning() || !m_send) {
        return;
    }
    CollabControlKeyframe keyframe;
    keyframe.keyframeEpoch = m_localEpoch;
    keyframe.deltaSeq = m_localDeltaSeq;
    keyframe.sessionTimeUs = 0;
    keyframe.values.reserve(static_cast<qsizetype>(m_entries.size()));
    for (const Entry& entry : m_entries) {
        keyframe.values.append(
                CollabControlValue{entry.id, entry.pProxy->get()});
    }
    m_send(CollabMessageType::ControlKeyframe,
            encodeCollabControlKeyframe(keyframe));
}

void CollabControlReplicator::flush() {
    if (!isRunning() || !m_send) {
        m_pFlushTimer->stop();
        return;
    }

    CollabControlDelta delta;
    delta.keyframeEpoch = m_localEpoch;
    for (Entry& entry : m_entries) {
        if (!entry.dirty) {
            continue;
        }
        const double value = entry.pProxy->get();
        entry.dirty = false;
        if (std::abs(value - entry.lastSentValue) < m_deadband) {
            continue;
        }
        entry.lastSentValue = value;
        delta.values.append(CollabControlValue{entry.id, value});
    }

    if (delta.values.isEmpty()) {
        m_pFlushTimer->stop();
        return;
    }
    delta.deltaSeq = ++m_localDeltaSeq;
    m_send(CollabMessageType::ControlDelta, encodeCollabControlDelta(delta));
}

void CollabControlReplicator::onLocalChange(int index, double value) {
    if (!isRunning() || m_applyingValue || index < 0 ||
            index >= static_cast<int>(m_entries.size())) {
        return;
    }
    Entry& entry = m_entries[static_cast<std::size_t>(index)];

    // An engine confirmation of a value we just applied remotely must not be
    // echoed back or reverted. A genuine local change is always newer than
    // the guard, so it falls through.
    if (entry.hasRemoteValue &&
            std::abs(value - entry.lastRemoteValue) < kEchoEpsilon &&
            m_pClock->nowUs() - entry.lastRemoteUs <
                    static_cast<qint64>(m_echoGuardMs) * 1000) {
        entry.lastSentValue = value;
        return;
    }

    if (!entry.localInputAllowed) {
        revertBlockedInput(&entry);
        return;
    }

    if (std::abs(value - entry.lastSentValue) < m_deadband) {
        return;
    }
    entry.dirty = true;
    if (isMomentaryControl(entry.sessionKey.item)) {
        flush();
        return;
    }
    m_pFlushTimer->start();
}

void CollabControlReplicator::revertBlockedInput(Entry* pEntry) {
    // Restore the last value the owner sent. Before the first delta arrives,
    // restore the value the control had when the session started.
    const double value =
            pEntry->hasRemoteValue ? pEntry->lastRemoteValue : pEntry->lastSentValue;
    // Setting through the proxy suppresses this revert from being observed as
    // a local change. The re-entrancy guard additionally stops control
    // cascades (loop start/end clamping, for example) from reverting each
    // other indefinitely.
    m_applyingValue = true;
    pEntry->pProxy->set(value);
    m_applyingValue = false;
    pEntry->lastRemoteUs = m_pClock->nowUs();
    emit localInputBlocked(pEntry->sessionDeck, pEntry->sessionKey.item);
}

void CollabControlReplicator::applyValue(Entry* pEntry, double value) {
    m_applyingValue = true;
    pEntry->pProxy->set(value);
    m_applyingValue = false;
    pEntry->dirty = false;
    pEntry->lastSentValue = value;
    pEntry->lastRemoteValue = value;
    pEntry->lastRemoteUs = m_pClock->nowUs();
    pEntry->hasRemoteValue = true;
}

CollabControlReplicator::Entry* CollabControlReplicator::entryForId(
        CollabControlId id) {
    for (Entry& entry : m_entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

bool CollabControlReplicator::handlePacket(const CollabPacket& packet) {
    if (!isRunning()) {
        return false;
    }

    if (packet.header.type == CollabMessageType::ControlDelta) {
        CollabControlDelta delta;
        if (!decodeCollabControlDelta(packet.payload, &delta)) {
            emit packetRejected(tr("Malformed control delta"));
            return true;
        }
        if (delta.keyframeEpoch != m_remoteEpoch) {
            emit packetRejected(tr("Control delta for an unknown keyframe epoch"));
            return true;
        }
        if (delta.deltaSeq <= m_lastRemoteDeltaSeq) {
            // Stale or duplicate datagram.
            return true;
        }
        for (const CollabControlValue& value : delta.values) {
            Entry* pEntry = entryForId(value.id);
            if (pEntry == nullptr) {
                emit packetRejected(
                        tr("Unknown control id %1").arg(value.id));
                continue;
            }
            applyValue(pEntry, value.value);
        }
        m_lastRemoteDeltaSeq = delta.deltaSeq;
        return true;
    }

    if (packet.header.type == CollabMessageType::ControlKeyframe) {
        CollabControlKeyframe keyframe;
        if (!decodeCollabControlKeyframe(packet.payload, &keyframe)) {
            emit packetRejected(tr("Malformed control keyframe"));
            return true;
        }
        if (keyframe.keyframeEpoch < m_remoteEpoch) {
            return true;
        }
        // The first keyframe of an epoch is always applied: its delta sequence
        // may be zero, which is not a stale value but the start of the stream.
        if (keyframe.keyframeEpoch == m_remoteEpoch &&
                m_remoteHasKeyframe &&
                keyframe.deltaSeq <= m_lastRemoteDeltaSeq) {
            return true;
        }
        m_remoteEpoch = keyframe.keyframeEpoch;
        m_remoteHasKeyframe = true;
        for (const CollabControlValue& value : keyframe.values) {
            Entry* pEntry = entryForId(value.id);
            if (pEntry == nullptr) {
                emit packetRejected(
                        tr("Unknown control id %1").arg(value.id));
                continue;
            }
            applyValue(pEntry, value.value);
        }
        m_lastRemoteDeltaSeq = keyframe.deltaSeq;
        emit keyframeApplied(keyframe.keyframeEpoch);
        return true;
    }

    return false;
}

} // namespace collab
} // namespace mixxx
