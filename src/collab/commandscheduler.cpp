#include "collab/commandscheduler.h"

#include <algorithm>
#include <utility>

namespace mixxx {
namespace collab {

bool CollabCommandScheduler::schedule(CommandId id,
        qint64 targetSessionUs,
        Callback callback) {
    if (id == 0 || !callback) {
        return false;
    }
    if (isKnown(id)) {
        return false;
    }
    // Session time starts at a positive host timestamp, so a negative target
    // is always a bug in the caller.
    if (targetSessionUs < 0) {
        return false;
    }

    for (const Entry& entry : m_pending) {
        if (entry.id == id) {
            return false;
        }
    }

    m_pending.push_back(Entry{id, targetSessionUs, std::move(callback)});
    return true;
}

bool CollabCommandScheduler::cancel(CommandId id) {
    const auto it = std::find_if(m_pending.begin(),
            m_pending.end(),
            [id](const Entry& entry) { return entry.id == id; });
    if (it == m_pending.end()) {
        return false;
    }
    m_pending.erase(it);
    return true;
}

int CollabCommandScheduler::runDue(qint64 sessionNowUs) {
    std::vector<Entry> due;
    std::vector<Entry> remaining;
    due.reserve(m_pending.size());
    remaining.reserve(m_pending.size());

    for (Entry& entry : m_pending) {
        if (entry.targetSessionUs <= sessionNowUs) {
            due.push_back(std::move(entry));
        } else {
            remaining.push_back(std::move(entry));
        }
    }
    m_pending = std::move(remaining);

    std::stable_sort(due.begin(),
            due.end(),
            [](const Entry& lhs, const Entry& rhs) {
                return lhs.targetSessionUs < rhs.targetSessionUs;
            });

    int executed = 0;
    for (Entry& entry : due) {
        const qint64 latenessUs = sessionNowUs - entry.targetSessionUs;
        rememberExecuted(entry.id);
        if (latenessUs > m_maxLatenessUs) {
            // Too late to be useful. It is still recorded as executed so a
            // retransmission is acknowledged instead of applied.
            continue;
        }
        if (entry.callback) {
            entry.callback(entry.id, latenessUs);
        }
        ++executed;
    }
    return executed;
}

void CollabCommandScheduler::clear() {
    m_pending.clear();
    m_executedOrder.clear();
}

bool CollabCommandScheduler::isKnown(CommandId id) const {
    if (std::find(m_executedOrder.begin(), m_executedOrder.end(), id) !=
            m_executedOrder.end()) {
        return true;
    }
    return std::any_of(m_pending.begin(),
            m_pending.end(),
            [id](const Entry& entry) { return entry.id == id; });
}

void CollabCommandScheduler::rememberExecuted(CommandId id) {
    m_executedOrder.push_back(id);
    if (static_cast<int>(m_executedOrder.size()) > kExecutedHistorySize) {
        m_executedOrder.erase(m_executedOrder.begin());
    }
}

} // namespace collab
} // namespace mixxx
