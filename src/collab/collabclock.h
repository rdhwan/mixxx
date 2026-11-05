#pragma once

#include <QtGlobal>
#include <memory>

namespace mixxx {
namespace collab {

/// Monotonic microsecond clock used by the collaboration layer.
///
/// The clock is injected so tests and the diagnostics harness can run without
/// waiting for real time and can simulate an offset or a clock skew between
/// two peers.
class CollabClock {
  public:
    virtual ~CollabClock() = default;

    /// Monotonic time in microseconds. Never decreases.
    virtual qint64 nowUs() const = 0;
};

/// Default clock backed by std::chrono::steady_clock.
class SteadyCollabClock : public CollabClock {
  public:
    qint64 nowUs() const override;
};

/// Clock with an artificial offset and skew on top of another clock.
///
/// `now = base + offsetUs + base * skewPpm / 1e6`. Used by the M0 harness to
/// validate that the estimator recovers a non-zero offset and skew, which a
/// plain loopback with one shared monotonic clock cannot do.
class SkewedCollabClock : public CollabClock {
  public:
    /// Takes ownership of the base clock when none is given.
    explicit SkewedCollabClock(
            qint64 offsetUs = 0,
            double skewPpm = 0.0,
            std::unique_ptr<CollabClock> pBase = nullptr);

    qint64 nowUs() const override;

    qint64 offsetUs() const {
        return m_offsetUs;
    }

    double skewPpm() const {
        return m_skewPpm;
    }

  private:
    qint64 m_offsetUs;
    double m_skewPpm;
    std::unique_ptr<CollabClock> m_pBase;
};

/// Manually advanced clock for unit tests.
class ManualCollabClock : public CollabClock {
  public:
    explicit ManualCollabClock(qint64 nowUs = 0)
            : m_nowUs(nowUs) {
    }

    qint64 nowUs() const override {
        return m_nowUs;
    }

    void setNowUs(qint64 nowUs) {
        m_nowUs = nowUs;
    }

    void advanceUs(qint64 deltaUs) {
        m_nowUs += deltaUs;
    }

  private:
    qint64 m_nowUs;
};

} // namespace collab
} // namespace mixxx
