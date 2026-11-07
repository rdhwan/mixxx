#include "collab/collabcontrolreplicator.h"

#include <QTimer>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <utility>

#include "collab/collabclock.h"
#include "control/controlproxy.h"
#include "moc_collabcontrolreplicator.cpp"

namespace mixxx {
namespace collab {

namespace {

/// Interval of the delta flush. Sixty per second is the protocol limit.
constexpr int kFlushIntervalMs = 16;

/// The peer may still be starting its replicator when the immediate keyframe
/// is sent, so send another one shortly after instead of waiting a full
/// keyframe interval.
constexpr int kInitialKeyframeDelayMs = 250;

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
            item == QStringLiteral("clear") ||
            item.startsWith(QStringLiteral("button_parameter")) ||
            (item.startsWith(QStringLiteral("hotcue_")) &&
                    item.endsWith(QStringLiteral("_activate")));
}

/// Matches `[EffectRack1_EffectUnit1_Effect2]` and
/// `[QuickEffectRack1_[Channel1]_Effect1]` but not their chain groups.
bool isEffectSlotGroup(const QString& group) {
    if (!group.startsWith(QStringLiteral("[EffectRack1_")) &&
            !group.startsWith(QStringLiteral("[QuickEffectRack1_"))) {
        return false;
    }
    const int position = group.lastIndexOf(QStringLiteral("_Effect"));
    if (position < 0) {
        return false;
    }
    QString number = group.mid(position + 7);
    if (!number.endsWith(QLatin1Char(']'))) {
        return false;
    }
    number.chop(1);
    bool ok = false;
    number.toInt(&ok);
    return ok && !number.isEmpty();
}

/// Effect chains, including the quick effect chains. The groups embed deck
/// channel names, which is why both peers must run the same 1:1 deck mapping.
bool isEffectChainGroup(const QString& group) {
    return group.startsWith(QStringLiteral("[EffectRack1_")) ||
            group.startsWith(QStringLiteral("[QuickEffectRack1_"));
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
    connect(m_pKeyframeTimer, &QTimer::timeout, this, [this]() {
        sendKeyframe();
        // The first keyframe goes out early in case the peer was still
        // starting; the rest keep the normal interval.
        if (m_pKeyframeTimer->interval() != m_keyframeIntervalMs) {
            m_pKeyframeTimer->start(m_keyframeIntervalMs);
        }
    });
}

CollabControlReplicator::~CollabControlReplicator() {
    stop();
}

namespace {

/// Returns the session deck of a session-space key, or zero for a key that is
/// not tied to one deck. Quick effect groups embed the deck channel name.
int sessionDeckForSessionKey(const ConfigKey& key) {
    const QString& group = key.group;
    const int plainChannelPosition =
            group.startsWith(QStringLiteral("[Channel")) ? 0 : -1;
    const int embeddedChannelPosition =
            group.startsWith(QStringLiteral("[QuickEffectRack1_[Channel"))
            ? group.indexOf(QStringLiteral("[Channel"))
            : -1;
    const int position = plainChannelPosition >= 0 ? plainChannelPosition
                                                   : embeddedChannelPosition;
    if (position < 0) {
        return 0;
    }
    bool ok = false;
    const int deck = group.mid(position + 8, 1).toInt(&ok);
    if (!ok || !isValidCollabDeckId(deck)) {
        return 0;
    }
    if (plainChannelPosition >= 0 &&
            group.size() != QStringLiteral("[Channel1]").size()) {
        return 0;
    }
    return deck;
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
        replicated.localInputAllowed = true;
        replicated.keyframeAuthority = true;
        if (enforceOwnership) {
            const bool isHost = deckMap.role() == CollabRole::Host;
            if (replicated.sessionDeck != 0) {
                // A deck and the quick effect chain behind it belong to the
                // peer that owns the deck.
                const bool owns =
                        deckMap.ownsSessionDeck(replicated.sessionDeck);
                replicated.localInputAllowed = owns;
                replicated.keyframeAuthority = owns;
            } else if (entry.key.group == QStringLiteral("[Master]")) {
                // The crossfader is host-owned.
                replicated.localInputAllowed = isHost;
                replicated.keyframeAuthority = isHost;
            } else if (isEffectChainGroup(entry.key.group)) {
                // Standard effect units are shared: either peer may move them,
                // and the host is the snapshot authority.
                replicated.localInputAllowed = true;
                replicated.keyframeAuthority = isHost;
            }
        }
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
    // Restarts (reconnect) must advance the epoch. The peer that stayed
    // connected keeps its delta sequence, so a repeated epoch would make the
    // fresh keyframe look stale.
    m_localEpoch = m_localEpoch == 0 ? 1 : m_localEpoch + 1;
    m_localDeltaSeq = 0;
    m_remoteEpoch = 1;
    m_lastRemoteDeltaSeq = 0;
    m_remoteHasKeyframe = false;
    m_pKeyframeTimer->start(kInitialKeyframeDelayMs);
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

    QList<CollabControlValue> allValues;
    allValues.reserve(static_cast<qsizetype>(m_entries.size()));
    for (const Entry& entry : m_entries) {
        if (!entry.keyframeAuthority) {
            // Both peers send keyframes, so a control may only appear in the
            // keyframe of its authority. Otherwise the snapshots would keep
            // overwriting each other every interval.
            continue;
        }
        allValues.append(CollabControlValue{entry.id, entry.pProxy->get()});
    }

    if (allValues.isEmpty()) {
        return;
    }

    const int total = static_cast<int>(allValues.size());
    const int chunkCount =
            std::max(1,
                    (total + kCollabMaxControlsPerPacket - 1) /
                            kCollabMaxControlsPerPacket);
    for (int chunk = 0; chunk < chunkCount; ++chunk) {
        const int begin = chunk * kCollabMaxControlsPerPacket;
        const int end = std::min(total, begin + kCollabMaxControlsPerPacket);

        CollabControlKeyframe keyframe;
        keyframe.keyframeEpoch = m_localEpoch;
        // Chunks of one snapshot must not look stale to the receiver, which
        // drops a keyframe whose delta sequence is not newer than the last.
        keyframe.deltaSeq = ++m_localDeltaSeq;
        keyframe.chunkIndex = static_cast<uint16_t>(chunk);
        keyframe.chunkCount = static_cast<uint16_t>(chunkCount);
        keyframe.sessionTimeUs = 0;
        keyframe.values = allValues.mid(begin, end - begin);

        if (!m_send(CollabMessageType::ControlKeyframe,
                    encodeCollabControlKeyframe(keyframe))) {
            return;
        }
    }
}

void CollabControlReplicator::flush() {
    if (!isRunning() || !m_send) {
        m_pFlushTimer->stop();
        return;
    }

    CollabControlDelta delta;
    delta.keyframeEpoch = m_localEpoch;
    QList<CollabControlValue> values;
    values.reserve(static_cast<qsizetype>(m_entries.size()));
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
        values.append(CollabControlValue{entry.id, value});
    }

    if (values.isEmpty()) {
        m_pFlushTimer->stop();
        return;
    }

    // A burst, such as loading an effect chain preset, can touch more controls
    // than one datagram holds. Every chunk gets its own sequence so the
    // receiver applies all of them.
    for (qsizetype begin = 0; begin < values.size();
            begin += kCollabMaxControlsPerPacket) {
        delta.deltaSeq = ++m_localDeltaSeq;
        delta.values = values.mid(begin, kCollabMaxControlsPerPacket);
        if (!m_send(CollabMessageType::ControlDelta,
                    encodeCollabControlDelta(delta))) {
            return;
        }
    }
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
    const QString& item = pEntry->sessionKey.item;

    if (item == QStringLiteral("loaded_effect") && value <= 0.0) {
        // EffectSlot ignores loaded_effect = 0, so unload through the slot's
        // clear button instead. A keyframe after a reconnect relies on this
        // to clear an effect the other peer no longer has.
        if (Entry* pClear = entryForSessionKey(pEntry->sessionKey.group,
                    QStringLiteral("clear"))) {
            m_applyingValue = true;
            pClear->pProxy->set(1.0);
            pClear->pProxy->set(0.0);
            m_applyingValue = false;
            pClear->dirty = false;
            pClear->lastSentValue = 0.0;
            pClear->lastRemoteValue = 0.0;
            pClear->lastRemoteUs = m_pClock->nowUs();
        }
    }

    // Re-applying an unchanged value is not just wasted work: loading the
    // effect that is already in a slot reloads it and resets its parameters,
    // and a keyframe repeats every mapped value.
    if (std::abs(pEntry->pProxy->get() - value) >= kEchoEpsilon) {
        m_applyingValue = true;
        pEntry->pProxy->set(value);
        m_applyingValue = false;
    }

    pEntry->dirty = false;
    pEntry->lastSentValue = value;
    pEntry->lastRemoteValue = value;
    pEntry->lastRemoteUs = m_pClock->nowUs();
    pEntry->hasRemoteValue = true;
}

int CollabControlReplicator::applyPriority(const Entry& entry) const {
    const QString& group = entry.sessionKey.group;
    const QString& item = entry.sessionKey.item;

    if (isEffectSlotGroup(group)) {
        if (item == QStringLiteral("clear")) {
            return 0;
        }
        if (item == QStringLiteral("loaded_effect")) {
            // Loading an effect resets its parameters, so it must come first.
            return 1;
        }
        if (item == QStringLiteral("meta")) {
            // The chain super knob writes every slot meta value. Apply it
            // before the individual meta values so the latter win.
            return 3;
        }
        if (item.startsWith(QStringLiteral("parameter")) ||
                item.startsWith(QStringLiteral("button_parameter"))) {
            return 4;
        }
        if (item == QStringLiteral("enabled")) {
            return 5;
        }
        return 6;
    }
    if (isEffectChainGroup(group)) {
        if (item == QStringLiteral("super1")) {
            return 2;
        }
        return 6;
    }

    if (item == QStringLiteral("loop_start_position")) {
        return 10;
    }
    if (item == QStringLiteral("loop_end_position")) {
        return 11;
    }
    if (item == QStringLiteral("loop_enabled")) {
        // LoopingControl rejects enabling a loop whose points are not valid
        // yet, so this must be applied last.
        return 12;
    }
    return 20;
}

void CollabControlReplicator::applyValues(
        const QList<CollabControlValue>& values) {
    using OrderedValue = std::pair<int, CollabControlValue>;
    QList<OrderedValue> ordered;
    ordered.reserve(values.size());
    for (const CollabControlValue& value : values) {
        Entry* pEntry = entryForId(value.id);
        if (pEntry == nullptr) {
            emit packetRejected(tr("Unknown control id %1").arg(value.id));
            continue;
        }
        ordered.append(OrderedValue{applyPriority(*pEntry), value});
    }
    std::stable_sort(ordered.begin(),
            ordered.end(),
            [](const OrderedValue& a, const OrderedValue& b) {
                return a.first < b.first;
            });
    for (const OrderedValue& orderedValue : ordered) {
        Entry* pEntry = entryForId(orderedValue.second.id);
        if (pEntry == nullptr) {
            continue;
        }
        applyValue(pEntry, orderedValue.second.value);
    }
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

CollabControlReplicator::Entry* CollabControlReplicator::entryForSessionKey(
        const QString& group, const QString& item) {
    for (Entry& entry : m_entries) {
        if (entry.sessionKey.group == group && entry.sessionKey.item == item) {
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
        applyValues(delta.values);
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
        applyValues(keyframe.values);
        m_lastRemoteDeltaSeq = keyframe.deltaSeq;
        emit keyframeApplied(keyframe.keyframeEpoch);
        return true;
    }

    return false;
}

} // namespace collab
} // namespace mixxx
