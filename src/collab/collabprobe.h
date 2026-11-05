#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>

#include "collab/clocksync.h"
#include "collab/collabclock.h"
#include "collab/collabdiagnostics.h"

class QTimer;
class QUdpSocket;

namespace mixxx {
namespace collab {

/// Minimal four-timestamp probe packet used by the M0 harness.
///
/// It deliberately carries only the fields the clock exchange needs. The full
/// protocol header is added in M1; the magic and version fields are already
/// present so the M1 parser can reject foreign traffic.
struct CollabProbePacket {
    enum class Type : quint8 {
        Ping = 1,
        Pong = 2,
    };

    Type type = Type::Ping;
    quint64 t1Us = 0;
    quint64 t2Us = 0;
    quint64 t3Us = 0;
};

QByteArray encodeCollabProbePacket(const CollabProbePacket& packet);
bool decodeCollabProbePacket(const QByteArray& data, CollabProbePacket* pPacket);

/// Symmetric UDP clock probe for M0 measurements.
///
/// Both peers can run a probe: each one sends pings to the other and answers
/// incoming pings with pongs. The resulting samples feed `CollabClockSync`.
/// The probe is not the M1 transport; it only measures RTT, offset, jitter,
/// and skew between two Mixxx instances.
class CollabProbe : public QObject {
    Q_OBJECT
  public:
    explicit CollabProbe(QObject* pParent = nullptr);
    ~CollabProbe() override;

    /// Binds `localPort` and starts pinging `peerAddress:peerPort`.
    /// A zero `localPort` lets the OS choose one.
    bool start(const QString& peerAddress,
            quint16 peerPort,
            quint16 localPort = 0,
            int intervalMs = 200);
    void stop();
    bool isRunning() const;

    QString errorString() const {
        return m_errorString;
    }

    QString localAddress() const;
    quint16 localPort() const;

    /// Local monotonic time used by the probe.
    qint64 nowUs() const {
        return m_pClock->nowUs();
    }

    const CollabClockSync& clockSync() const {
        return m_clockSync;
    }

    CollabDiagnostics* diagnostics() {
        return &m_diagnostics;
    }

    qint64 offsetUs() const {
        return m_clockSync.offsetUs();
    }
    qint64 rttUs() const {
        return m_clockSync.rttUs();
    }
    qint64 jitterUs() const {
        return m_clockSync.jitterUs();
    }
    double skewPpm() const {
        return m_clockSync.skewPpm();
    }
    int sampleCount() const {
        return m_clockSync.sampleCount();
    }

    /// Drops every clock sample and resets the estimate.
    void clearStatistics();

  signals:
    void statisticsChanged();
    void probeError(const QString& message);

  private:
    void handleDatagram(const QByteArray& data, const QHostAddress& sender, quint16 senderPort);
    void sendPing();

    std::unique_ptr<SteadyCollabClock> m_pClock;
    CollabClockSync m_clockSync;
    CollabDiagnostics m_diagnostics;
    QUdpSocket* m_pSocket = nullptr;
    QTimer* m_pTimer = nullptr;
    QHostAddress m_peerAddress;
    quint16 m_peerPort = 0;
    QString m_errorString;
    bool m_running = false;
};

} // namespace collab
} // namespace mixxx
