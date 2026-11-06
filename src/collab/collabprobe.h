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
        PlayCommand = 3,
    };

    Type type = Type::Ping;
    quint64 t1Us = 0;
    quint64 t2Us = 0;
    quint64 t3Us = 0;
};

/// A scheduled play command carried over the probe socket.
///
/// The target is in the session-time domain (host monotonic time). The
/// receiver schedules the command in its own session time, so no conversion is
/// needed on the sending side.
struct CollabPlayCommand {
    quint64 commandId = 0;
    qint64 targetSessionUs = 0;
    quint8 deck = 0;
};

QByteArray encodeCollabProbePacket(const CollabProbePacket& packet);
bool decodeCollabProbePacket(const QByteArray& data, CollabProbePacket* pPacket);
QByteArray encodeCollabPlayCommand(const CollabPlayCommand& command);
bool decodeCollabPlayCommand(const QByteArray& data, CollabPlayCommand* pCommand);

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

    /// True when a pong from the peer arrived within the liveness window
    /// (three probe intervals, at least one second). A running probe with no
    /// pongs is not connected; it is only waiting for the peer.
    bool isPeerConnected() const {
        return m_peerConnected;
    }

    /// Milliseconds since the last pong, or -1 before the first one.
    int lastResponseAgeMs() const;

    /// Sends a scheduled play command to the peer. Returns false when the
    /// probe is not running or the datagram could not be written.
    bool sendPlayCommand(quint64 commandId, qint64 targetSessionUs, quint8 deck);

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
    /// Emitted when the peer starts or stops answering the probe.
    void peerConnectedChanged(bool connected);
    /// A scheduled play command from the peer.
    void playCommandReceived(quint64 commandId, qint64 targetSessionUs, int deck);

  private:
    void handleDatagram(const QByteArray& data, const QHostAddress& sender, quint16 senderPort);
    void sendPing();
    void updateConnectionState();
    void setPeerConnected(bool connected);

    std::unique_ptr<SteadyCollabClock> m_pClock;
    CollabClockSync m_clockSync;
    CollabDiagnostics m_diagnostics;
    QUdpSocket* m_pSocket = nullptr;
    QTimer* m_pTimer = nullptr;
    QTimer* m_pLivenessTimer = nullptr;
    QHostAddress m_peerAddress;
    quint16 m_peerPort = 0;
    int m_intervalMs = 200;
    qint64 m_lastResponseUs = 0;
    QString m_errorString;
    bool m_running = false;
    bool m_peerConnected = false;
};

} // namespace collab
} // namespace mixxx
