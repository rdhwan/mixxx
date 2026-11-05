#include "collab/clocksync.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mixxx {
namespace collab {

namespace {

qint64 median(std::vector<qint64> values) {
    if (values.empty()) {
        return 0;
    }
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    qint64 result = *middle;
    if ((values.size() % 2) == 0) {
        const auto lower = std::max_element(values.begin(), middle);
        result = (*lower + result) / 2;
    }
    return result;
}

} // namespace

bool CollabClockSync::addSample(qint64 t1Us, qint64 t2Us, qint64 t3Us, qint64 t4Us) {
    const qint64 remoteTurnaroundUs = t3Us - t2Us;
    const qint64 localTurnaroundUs = t4Us - t1Us;
    if (remoteTurnaroundUs < 0 || localTurnaroundUs < 0) {
        return false;
    }
    const qint64 rttUs = localTurnaroundUs - remoteTurnaroundUs;
    if (rttUs < 0) {
        return false;
    }

    CollabClockSample sample;
    sample.t1Us = t1Us;
    sample.t2Us = t2Us;
    sample.t3Us = t3Us;
    sample.t4Us = t4Us;
    sample.rttUs = rttUs;
    sample.offsetUs = ((t2Us - t1Us) + (t3Us - t4Us)) / 2;
    sample.midUs = t1Us + localTurnaroundUs / 2;

    m_samples.push_back(sample);
    while (static_cast<int>(m_samples.size()) > kMaxSamples) {
        m_samples.pop_front();
    }

    recompute();
    return true;
}

void CollabClockSync::reset() {
    m_samples.clear();
    m_accepted.clear();
    m_hasEstimate = false;
    m_offsetUs = 0;
    m_rttUs = 0;
    m_jitterUs = 0;
    m_skewPpm = 0.0;
    m_referenceUs = 0;
}

void CollabClockSync::recompute() {
    if (m_samples.empty()) {
        reset();
        return;
    }

    qint64 minRttUs = m_samples.front().rttUs;
    for (const CollabClockSample& sample : m_samples) {
        minRttUs = std::min(minRttUs, sample.rttUs);
    }
    const qint64 acceptThresholdUs = minRttUs +
            std::max<qint64>(kMinRttSlackUs, minRttUs / 2);

    m_accepted.clear();
    qint64 rttSumUs = 0;
    std::vector<qint64> offsets;
    offsets.reserve(m_samples.size());
    for (const CollabClockSample& sample : m_samples) {
        if (sample.rttUs > acceptThresholdUs) {
            continue;
        }
        m_accepted.push_back(sample);
        rttSumUs += sample.rttUs;
        offsets.push_back(sample.offsetUs);
    }

    if (m_accepted.empty()) {
        // Every sample passed the RTT check at the time it was added but the
        // minimum may have moved. Fall back to the newest sample.
        m_accepted.push_back(m_samples.back());
        offsets.push_back(m_samples.back().offsetUs);
        rttSumUs = m_samples.back().rttUs;
    }

    m_hasEstimate = true;
    m_rttUs = minRttUs;
    m_offsetUs = median(offsets);
    m_referenceUs = m_accepted.back().midUs;

    const qint64 meanRttUs = rttSumUs / static_cast<qint64>(m_accepted.size());
    qint64 deviationSumUs = 0;
    for (const CollabClockSample& sample : m_accepted) {
        deviationSumUs += std::abs(sample.rttUs - meanRttUs);
    }
    m_jitterUs = deviationSumUs / static_cast<qint64>(m_accepted.size());

    m_skewPpm = 0.0;
    if (static_cast<int>(m_accepted.size()) >= kMinSamplesForSkew) {
        // Linear regression of the offset over local time.
        double meanTime = 0.0;
        double meanOffset = 0.0;
        for (const CollabClockSample& sample : m_accepted) {
            meanTime += static_cast<double>(sample.midUs);
            meanOffset += static_cast<double>(sample.offsetUs);
        }
        const double count = static_cast<double>(m_accepted.size());
        meanTime /= count;
        meanOffset /= count;

        double covariance = 0.0;
        double variance = 0.0;
        for (const CollabClockSample& sample : m_accepted) {
            const double dt = static_cast<double>(sample.midUs) - meanTime;
            const double dOffset = static_cast<double>(sample.offsetUs) - meanOffset;
            covariance += dt * dOffset;
            variance += dt * dt;
        }

        if (variance > 0.0) {
            const double slope = covariance / variance;
            const double skewPpm = slope * 1000000.0;
            if (std::isfinite(skewPpm) &&
                    std::abs(skewPpm) <= kMaxPlausibleSkewPpm) {
                m_skewPpm = skewPpm;
                // The median offset is anchored at the sample midpoint, not
                // at the reference time. Move it along the regression line so
                // `offsetUs()` and `skewPpm()` describe the same instant.
                m_offsetUs = static_cast<qint64>(std::llround(
                        meanOffset + slope * (static_cast<double>(m_referenceUs) - meanTime)));
            }
        }
    }
}

qint64 CollabClockSync::toRemoteTime(qint64 localUs) const {
    if (!m_hasEstimate) {
        return localUs;
    }
    const double elapsedUs = static_cast<double>(localUs - m_referenceUs);
    const qint64 skewUs = static_cast<qint64>(
            elapsedUs * m_skewPpm / 1000000.0);
    return localUs + m_offsetUs + skewUs;
}

qint64 CollabClockSync::toLocalTime(qint64 remoteUs) const {
    if (!m_hasEstimate) {
        return remoteUs;
    }
    const double scale = 1.0 + m_skewPpm / 1000000.0;
    const double localUs =
            (static_cast<double>(remoteUs) - static_cast<double>(m_offsetUs) +
                    static_cast<double>(m_referenceUs) * m_skewPpm / 1000000.0) /
            scale;
    return static_cast<qint64>(std::llround(localUs));
}

} // namespace collab
} // namespace mixxx
