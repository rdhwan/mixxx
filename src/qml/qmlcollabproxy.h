#pragma once

#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>
#include <QString>

#include "preferences/usersettings.h"

namespace mixxx {
namespace qml {

/// QML singleton that exposes the Online B2B session.
///
/// The session itself lives in `mixxx::collab::CollabConnection`, which is
/// shared by every QML engine in the process. This class only holds the
/// user-editable session settings and forwards state and invokables to the
/// connection.
class QmlCollabProxy : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool clockReady READ clockReady NOTIFY statisticsChanged)
    Q_PROPERTY(QString peerAddress READ peerAddress NOTIFY connectedChanged)
    Q_PROPERTY(QString peerHost READ peerHost WRITE setPeerHost NOTIFY
                    peerHostChanged)
    Q_PROPERTY(int peerPort READ peerPort WRITE setPeerPort NOTIFY
                    peerPortChanged)
    Q_PROPERTY(int localPort READ localPort WRITE setLocalPort NOTIFY
                    localPortChanged)
    Q_PROPERTY(int intervalMs READ intervalMs WRITE setIntervalMs NOTIFY
                    intervalMsChanged)
    Q_PROPERTY(QString sessionCode READ sessionCode WRITE setSessionCode NOTIFY
                    sessionCodeChanged)
    Q_PROPERTY(QString diagnosticsPath READ diagnosticsPath WRITE
                    setDiagnosticsPath NOTIFY diagnosticsPathChanged)
    Q_PROPERTY(double rttMs READ rttMs NOTIFY statisticsChanged)
    Q_PROPERTY(double offsetMs READ offsetMs NOTIFY statisticsChanged)
    Q_PROPERTY(double jitterMs READ jitterMs NOTIFY statisticsChanged)
    Q_PROPERTY(double skewPpm READ skewPpm NOTIFY statisticsChanged)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY statisticsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
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

    bool sessionActive() const;
    bool connected() const;
    bool clockReady() const;
    /// Address of the connected peer, empty before the handshake.
    QString peerAddress() const;
    QString peerHost() const;
    void setPeerHost(const QString& peerHost);
    int peerPort() const;
    void setPeerPort(int peerPort);
    int localPort() const;
    void setLocalPort(int localPort);
    int intervalMs() const;
    void setIntervalMs(int intervalMs);
    QString sessionCode() const;
    void setSessionCode(const QString& code);
    QString diagnosticsPath() const;
    void setDiagnosticsPath(const QString& path);

    double rttMs() const;
    double offsetMs() const;
    double jitterMs() const;
    double skewPpm() const;
    int sampleCount() const;
    QString lastError() const;
    int lastResponseAgeMs() const;

    /// True when this instance defines session time. The guest maps its local
    /// clock onto the host clock through the estimate.
    bool hostRole() const;
    void setHostRole(bool hostRole);

    bool scheduledCommandValid() const;
    double lastCommandLatenessMs() const;
    int lastCommandDeck() const;
    /// True when the last play command was received from the peer.
    bool lastCommandRemote() const;

    /// Hosts a session, or joins one, depending on `hostRole`.
    Q_INVOKABLE bool startSession();
    Q_INVOKABLE void stopSession();
    Q_INVOKABLE void clearStatistics();

    /// Schedules a play on both peers at the next two-second boundary in
    /// session time.
    Q_INVOKABLE bool schedulePlayTest(int deck, int leadMs = 3000);
    Q_INVOKABLE void cancelScheduledCommands();

    /// Generates and stores a new random session code.
    Q_INVOKABLE QString newSessionCode();

    /// Default CSV location inside the Mixxx settings directory.
    Q_INVOKABLE QString defaultDiagnosticsPath() const;

  signals:
    void sessionActiveChanged();
    void connectedChanged();
    void peerHostChanged();
    void peerPortChanged();
    void localPortChanged();
    void intervalMsChanged();
    void sessionCodeChanged();
    void diagnosticsPathChanged();
    void statisticsChanged();
    void lastErrorChanged();
    void hostRoleChanged();
    void lastCommandChanged();

  private:
    void onConnectionStateChanged();
    void onStatisticsChanged();
    void onPlayApplied(int deck, double latenessMs, bool remote);
    void loadSettings();
    void storeValue(const QString& key, const QVariant& value);
    QVariant loadValue(const QString& key, const QVariant& defaultValue) const;
    void setLastError(const QString& error);
    void applyRolePorts();

    static inline UserSettingsPointer s_pConfig = nullptr;

    int m_lastCommandDeck = 0;
    double m_lastCommandLatenessMs = 0.0;
    bool m_scheduledCommandValid = false;
    bool m_lastCommandRemote = false;
    bool m_hostRole = true;
    bool m_lastConnected = false;
    bool m_lastActive = false;

    QString m_peerHost;
    int m_peerPort = 0;
    int m_localPort = 0;
    int m_intervalMs = 200;
    QString m_sessionCode;
    QString m_diagnosticsPath;
    QString m_lastError;
};

} // namespace qml
} // namespace mixxx
