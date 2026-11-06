#pragma once

#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <memory>

#include "collab/collabprobe.h"
#include "collab/commandscheduler.h"
#include "preferences/usersettings.h"

class ControlProxy;
class QTimer;

namespace mixxx {
namespace qml {

/// QML singleton that exposes the M0 collaboration diagnostics probe.
///
/// This is the first user-visible part of the collaboration feature: it can
/// run a UDP clock probe against a second Mixxx instance, show RTT, offset,
/// jitter, and skew, and write the samples to a CSV file.
class QmlCollabProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool probeRunning READ probeRunning NOTIFY probeRunningChanged)
    Q_PROPERTY(QString peerHost READ peerHost WRITE setPeerHost NOTIFY
                    peerHostChanged)
    Q_PROPERTY(int peerPort READ peerPort WRITE setPeerPort NOTIFY
                    peerPortChanged)
    Q_PROPERTY(int localPort READ localPort WRITE setLocalPort NOTIFY
                    localPortChanged)
    Q_PROPERTY(int intervalMs READ intervalMs WRITE setIntervalMs NOTIFY
                    intervalMsChanged)
    Q_PROPERTY(QString diagnosticsPath READ diagnosticsPath WRITE
                    setDiagnosticsPath NOTIFY diagnosticsPathChanged)
    Q_PROPERTY(double rttMs READ rttMs NOTIFY statisticsChanged)
    Q_PROPERTY(double offsetMs READ offsetMs NOTIFY statisticsChanged)
    Q_PROPERTY(double jitterMs READ jitterMs NOTIFY statisticsChanged)
    Q_PROPERTY(double skewPpm READ skewPpm NOTIFY statisticsChanged)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY statisticsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(int lastResponseAgeMs READ lastResponseAgeMs NOTIFY
                    statisticsChanged)
    Q_PROPERTY(bool hostRole READ hostRole WRITE setHostRole NOTIFY
                    hostRoleChanged)
    Q_PROPERTY(bool scheduledCommandValid READ scheduledCommandValid NOTIFY
                    lastCommandChanged)
    Q_PROPERTY(double lastCommandLatenessMs READ lastCommandLatenessMs NOTIFY
                    lastCommandChanged)
    Q_PROPERTY(int lastCommandDeck READ lastCommandDeck NOTIFY
                    lastCommandChanged)
    Q_PROPERTY(bool lastCommandRemote READ lastCommandRemote NOTIFY
                    lastCommandChanged)
    QML_NAMED_ELEMENT(Collab)
    QML_SINGLETON
  public:
    explicit QmlCollabProxy(QObject* pParent = nullptr);
    ~QmlCollabProxy() override;

    static QmlCollabProxy* create(QQmlEngine* pQmlEngine,
            [[maybe_unused]] QJSEngine* pJsEngine) {
        return new QmlCollabProxy(pQmlEngine);
    }

    static void registerUserSettings(UserSettingsPointer pConfig) {
        s_pConfig = std::move(pConfig);
    }

    bool probeRunning() const;
    QString peerHost() const;
    void setPeerHost(const QString& peerHost);
    int peerPort() const;
    void setPeerPort(int peerPort);
    int localPort() const;
    void setLocalPort(int localPort);
    int intervalMs() const;
    void setIntervalMs(int intervalMs);
    QString diagnosticsPath() const;
    void setDiagnosticsPath(const QString& path);

    double rttMs() const;
    double offsetMs() const;
    double jitterMs() const;
    double skewPpm() const;
    int sampleCount() const;
    QString lastError() const;
    /// True when the peer answers the probe within the liveness window.
    bool connected() const;
    int lastResponseAgeMs() const;

    /// True when this instance defines session time. The guest maps its local
    /// clock onto the host clock through the probe estimate.
    bool hostRole() const;
    void setHostRole(bool hostRole);

    bool scheduledCommandValid() const;
    double lastCommandLatenessMs() const;
    int lastCommandDeck() const;
    /// True when the last play command was received from the peer.
    bool lastCommandRemote() const;

    /// Starts the probe with the current settings.
    Q_INVOKABLE bool startProbe();
    Q_INVOKABLE void stopProbe();
    Q_INVOKABLE void clearStatistics();

    /// Schedules a play command on both peers at the next two-second boundary
    /// in session time. The local deck is scheduled and the command is sent to
    /// the connected peer, which schedules the same target in its own session
    /// time.
    Q_INVOKABLE bool schedulePlayTest(int deck, int leadMs = 3000);
    Q_INVOKABLE void cancelScheduledCommands();

    /// Default CSV location inside the Mixxx settings directory.
    Q_INVOKABLE QString defaultDiagnosticsPath() const;

  signals:
    void probeRunningChanged();
    void peerHostChanged();
    void peerPortChanged();
    void localPortChanged();
    void intervalMsChanged();
    void diagnosticsPathChanged();
    void statisticsChanged();
    void lastErrorChanged();
    void connectedChanged();
    void hostRoleChanged();
    void lastCommandChanged();

  private:
    void loadSettings();
    void storeValue(const QString& key, const QVariant& value);
    QVariant loadValue(const QString& key, const QVariant& defaultValue) const;
    void setLastError(const QString& error);
    qint64 sessionNowUs() const;
    void tickScheduler();
    void applyRolePorts();
    quint64 makeCommandId();
    bool scheduleLocalPlay(
            int deck, qint64 targetSessionUs, quint64 commandId, bool remote);
    void onPlayCommandReceived(
            quint64 commandId, qint64 targetSessionUs, int deck);

    static inline UserSettingsPointer s_pConfig = nullptr;

    collab::CollabProbe* m_pProbe = nullptr;
    collab::CollabCommandScheduler m_scheduler;
    QTimer* m_pSchedulerTimer = nullptr;
    quint64 m_nextCommandId = 1;
    int m_lastCommandDeck = 0;
    double m_lastCommandLatenessMs = 0.0;
    bool m_scheduledCommandValid = false;
    bool m_lastCommandRemote = false;
    bool m_hostRole = true;

    QString m_peerHost;
    int m_peerPort = 0;
    int m_localPort = 0;
    int m_intervalMs = 200;
    QString m_diagnosticsPath;
    QString m_lastError;
};

} // namespace qml
} // namespace mixxx
