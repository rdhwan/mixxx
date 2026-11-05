#include "collab/collabclock.h"

#include <chrono>
#include <cmath>

namespace mixxx {
namespace collab {

namespace {

qint64 steadyNowUs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

} // namespace

qint64 SteadyCollabClock::nowUs() const {
    return steadyNowUs();
}

SkewedCollabClock::SkewedCollabClock(qint64 offsetUs,
        double skewPpm,
        std::unique_ptr<CollabClock> pBase)
        : m_offsetUs(offsetUs),
          m_skewPpm(skewPpm),
          m_pBase(pBase ? std::move(pBase)
                        : std::make_unique<SteadyCollabClock>()) {
}

qint64 SkewedCollabClock::nowUs() const {
    const qint64 baseUs = m_pBase->nowUs();
    const auto skewUs = static_cast<qint64>(std::llround(
            static_cast<double>(baseUs) * m_skewPpm / 1000000.0));
    return baseUs + m_offsetUs + skewUs;
}

} // namespace collab
} // namespace mixxx
