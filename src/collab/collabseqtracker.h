#pragma once

#include <QtGlobal>
#include <cstdint>

namespace mixxx {
namespace collab {

/// Estimates datagram loss from the wire sequence numbers.
///
/// The sequence starts at 1 and increases by one per sent datagram. A 64-entry
/// sliding window tolerates reordering: a packet that arrives late but still
/// inside the window is no longer counted as lost. A packet that falls out of
/// the window without arriving stays lost for good.
///
/// A gap is counted as soon as it appears, so a peer that goes silent after one
/// late datagram still reports the loss. This is a diagnostics aid, not a
/// reliability layer: nothing is retransmitted on the basis of these counters.
class CollabSequenceTracker {
  public:
    static constexpr int kWindowSize = 64;

    void reset() {
        m_highestSeq = 0;
        m_window = 0;
        m_receivedCount = 0;
        m_lostCount = 0;
    }

    /// Records one received sequence number. Duplicates and packets older than
    /// the window are ignored.
    void note(uint32_t seq) {
        if (seq == 0) {
            // The wire sequence starts at 1, so zero means the sender did not
            // set one.
            return;
        }
        if (m_highestSeq == 0) {
            m_highestSeq = seq;
            m_window = 1;
            m_receivedCount = 1;
            return;
        }
        if (seq > m_highestSeq) {
            const uint32_t delta = seq - m_highestSeq;
            if (delta > 1) {
                // Every skipped sequence is missing right now. A packet that
                // arrives late is subtracted again below.
                m_lostCount += delta - 1;
            }
            if (delta >= static_cast<uint32_t>(kWindowSize)) {
                m_window = 0;
            } else {
                m_window <<= delta;
            }
            m_window |= 1;
            m_highestSeq = seq;
            ++m_receivedCount;
            return;
        }

        const uint32_t age = m_highestSeq - seq;
        if (age >= static_cast<uint32_t>(kWindowSize)) {
            // Outside the window: it stays counted as lost.
            return;
        }
        const uint64_t bit = uint64_t{1} << age;
        if ((m_window & bit) != 0) {
            return;
        }
        m_window |= bit;
        ++m_receivedCount;
        // Every unset bit inside the window is a gap that was already counted,
        // so an in-window arrival is a reordered packet, not a loss.
        if (m_lostCount > 0) {
            --m_lostCount;
        }
    }

    /// Returns true when `seq` was already recorded inside the window.
    /// Retransmitted reliable datagrams reuse their sequence number, so a
    /// set bit means this datagram was delivered before.
    bool seen(uint32_t seq) const {
        if (seq == 0 || m_highestSeq == 0 || seq > m_highestSeq) {
            return false;
        }
        const uint32_t age = m_highestSeq - seq;
        if (age >= static_cast<uint32_t>(kWindowSize)) {
            return false;
        }
        return (m_window & (uint64_t{1} << age)) != 0;
    }

    quint64 receivedCount() const {
        return m_receivedCount;
    }
    quint64 lostCount() const {
        return m_lostCount;
    }

    /// Lost packets as a percentage of the packets that should have arrived.
    /// Zero before the first packet.
    double lossPercent() const {
        const quint64 total = m_receivedCount + m_lostCount;
        if (total == 0) {
            return 0.0;
        }
        return 100.0 * static_cast<double>(m_lostCount) /
                static_cast<double>(total);
    }

  private:
    uint32_t m_highestSeq = 0;
    /// Bit 0 is `m_highestSeq`, bit 1 is one older, and so on. A set bit means
    /// that sequence arrived; a clear bit inside the window is a counted gap.
    uint64_t m_window = 0;
    quint64 m_receivedCount = 0;
    quint64 m_lostCount = 0;
};

} // namespace collab
} // namespace mixxx
