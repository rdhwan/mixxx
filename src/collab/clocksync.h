#pragma once

#include <QtGlobal>

#include "collab/collabclock.h"
#include <deque>
#include <optional>

namespace mixxx {
namespace collab {

/// One completed four-timestamp exchange.
///
/// `t1`/`t4` are local monotonic times, `t2`/`t3` are remote monotonic times.
/// The remote clock is `local + offset`, so a positive offset means the remote
/// clock is ahead.
struct CollabClockSample {
    qint64 t1Us = 0;
    qint64 t2Us = 0;
    qint64 t3Us = 0;
    qint64 t4Us = 0;

    /// Round-trip time excluding the remote processing time.
    qint64 rttUs = 0;
    /// Remote minus local clock offset at the sample midpoint.
    qint64 offsetUs = 0;
    /// Local time at the sample midpoint.
    qint64 midUs = 0;
};

/// NTP-style clock synchronisation between two peers.
///
/// Samples are filtered by round-trip time, the offset is the median of the
/// best samples, and the skew is a linear regression of the offset over time.
/// The estimate is intentionally conservative: it returns `false` from
/// `hasEstimate()` until at least one consistent sample exists.
class CollabClockSync {
  public:
    /// Maximum number of stored samples. Older samples are dropped.
    static constexpr int kMaxSamples = 64;
    /// Minimum number of accepted samples before skew is estimated.
    static constexpr int kMinSamplesForSkew = 8;
    /// Samples slower than `minRtt + max(1 ms, minRtt / 2)` are ignored when
    /// computing the offset. A single congested exchange must not move the
    /// estimate.
    static constexpr qint64 kMinRttSlackUs = 1000;
    /// Implausible skews are ignored. Normal audio devices drift by tens of
    /// ppm, not thousands.
    static constexpr double kMaxPlausibleSkewPpm = 1000.0;

    /// Adds a completed exchange. Returns false when the timestamps are
    /// inconsistent (for example a negative round-trip time).
    bool addSample(qint64 t1Us, qint64 t2Us, qint64 t3Us, qint64 t4Us);

    void reset();

    int sampleCount() const {
        return static_cast<int>(m_samples.size());
    }

    bool hasEstimate() const {
        return m_hasEstimate;
    }

    /// Median offset of the best samples, remote minus local.
    qint64 offsetUs() const {
        return m_offsetUs;
    }

    /// Smallest observed round-trip time. Zero when no sample exists.
    qint64 rttUs() const {
        return m_rttUs;
    }

    /// Mean absolute deviation of the accepted round-trip times.
    qint64 jitterUs() const {
        return m_jitterUs;
    }

    /// Offset drift in parts per million. Zero until enough samples exist.
    double skewPpm() const {
        return m_skewPpm;
    }

    /// Number of samples used for the current offset estimate.
    int acceptedSampleCount() const {
        return static_cast<int>(m_accepted.size());
    }

    /// Most recent raw sample, if any. Useful for diagnostics logging.
    const CollabClockSample* lastSample() const {
        return m_samples.empty() ? nullptr : &m_samples.back();
    }

    /// Converts a local monotonic timestamp to the remote clock domain.
    qint64 toRemoteTime(qint64 localUs) const;

    /// Converts a remote timestamp to the local monotonic clock domain.
    qint64 toLocalTime(qint64 remoteUs) const;

  private:
    void recompute();

    std::deque<CollabClockSample> m_samples;
    std::deque<CollabClockSample> m_accepted;
    bool m_hasEstimate = false;
    qint64 m_offsetUs = 0;
    qint64 m_rttUs = 0;
    qint64 m_jitterUs = 0;
    double m_skewPpm = 0.0;
    /// Local time for which `m_offsetUs` is exact.
    qint64 m_referenceUs = 0;
};

/// Helper that turns a remote-clock estimate plus an injected local clock into
/// the session time domain. The host defines session time; the guest maps its
/// local clock onto it.
class CollabSessionClock {
  public:
    CollabSessionClock(const CollabClock* pClock, const CollabClockSync* pSync)
            : m_pClock(pClock),
              m_pSync(pSync) {
    }

    qint64 localNowUs() const {
        return m_pClock->nowUs();
    }

    qint64 sessionNowUs() const {
        return m_pSync->toRemoteTime(m_pClock->nowUs());
    }

    qint64 sessionToLocalUs(qint64 sessionUs) const {
        return m_pSync->toLocalTime(sessionUs);
    }

  private:
    const CollabClock* m_pClock;
    const CollabClockSync* m_pSync;
};

} // namespace collab
} // namespace mixxx
