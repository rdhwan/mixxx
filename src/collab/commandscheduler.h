#pragma once

#include <QtGlobal>
#include <functional>
#include <vector>

namespace mixxx {
namespace collab {

/// Executes commands at a target time in the session clock domain.
///
/// The scheduler never sleeps and never owns a timer thread. The caller drives
/// it with `runDue()` from the GUI or a network thread, passing the current
/// session time. This keeps it deterministic in tests and avoids adding a
/// thread to the engine.
class CollabCommandScheduler {
  public:
    /// Wire command identifier, unique per sender per session.
    using CommandId = quint64;

    /// Called when a command is due.
    ///
    /// `latenessUs` is zero when the command fires inside its tolerance and
    /// positive when the scheduler was driven late. The caller decides whether
    /// a late command still makes sense (for example `PLAY` may seek forward
    /// to the expected position before starting).
    using Callback = std::function<void(CommandId, qint64 latenessUs)>;

    /// Default tolerance. Beyond this the command is dropped instead of being
    /// applied at a position the other peer no longer expects.
    static constexpr qint64 kDefaultMaxLatenessUs = 250000;
    /// Number of executed identifiers remembered for duplicate suppression.
    static constexpr int kExecutedHistorySize = 4096;

    void setMaxLatenessUs(qint64 maxLatenessUs) {
        m_maxLatenessUs = maxLatenessUs;
    }

    qint64 maxLatenessUs() const {
        return m_maxLatenessUs;
    }

    /// Schedules a command at a session timestamp.
    ///
    /// Returns false for a duplicate identifier, a callback-less command, or a
    /// target that is already older than the lateness tolerance.
    bool schedule(CommandId id, qint64 targetSessionUs, Callback callback);

    /// Cancels a pending command. Returns false when it is not pending.
    bool cancel(CommandId id);

    /// Executes every command whose target is at or before `sessionNowUs`.
    /// Commands are executed in target order. Returns the number executed.
    int runDue(qint64 sessionNowUs);

    /// Discards every pending command and the duplicate history.
    void clear();

    /// Resets the executed/dropped counters without touching pending commands.
    void resetStatistics() {
        m_executedCount = 0;
        m_droppedCount = 0;
        m_maxObservedLatenessUs = 0;
    }

    int pendingCount() const {
        return static_cast<int>(m_pending.size());
    }

    /// Commands that ran inside the lateness tolerance since the last clear.
    quint64 executedCount() const {
        return m_executedCount;
    }
    /// Commands that were dropped because they were already too late.
    quint64 droppedCount() const {
        return m_droppedCount;
    }
    /// Largest lateness observed on an executed command since the last clear.
    qint64 maxObservedLatenessUs() const {
        return m_maxObservedLatenessUs;
    }

    /// True when the identifier was already executed or is pending.
    bool isKnown(CommandId id) const;

    /// Convenience helper for the sender: target = now + lead.
    static qint64 targetFromLead(qint64 sessionNowUs, qint64 leadUs) {
        return sessionNowUs + leadUs;
    }

  private:
    struct Entry {
        CommandId id = 0;
        qint64 targetSessionUs = 0;
        Callback callback;
    };

    void rememberExecuted(CommandId id);

    std::vector<Entry> m_pending;
    std::vector<CommandId> m_executedOrder;
    qint64 m_maxLatenessUs = kDefaultMaxLatenessUs;
    quint64 m_executedCount = 0;
    quint64 m_droppedCount = 0;
    qint64 m_maxObservedLatenessUs = 0;
};

} // namespace collab
} // namespace mixxx
