#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <map>
#include <memory>

#include "collab/trackcache.h"
#include "collab/trackhash.h"

class QFile;
class QTcpServer;
class QTcpSocket;
class QTimer;

namespace mixxx {
namespace collab {

/// TCP bulk channel for track files.
///
/// Each peer runs a server. A receiver connects to the sender's server and
/// asks for one content hash. The sender streams the file from the offset the
/// receiver already has, so an interrupted transfer resumes instead of
/// restarting. The receiver writes into the cache's partial directory and only
/// installs the file after the full SHA-256 matches.
///
/// The service is not a security boundary. It is meant to run only while an
/// authenticated collaboration session is active, and the caller should
/// restrict it to the peer address.
class CollabTransferService : public QObject {
    Q_OBJECT
  public:
    explicit CollabTransferService(
            CollabTrackCache* pCache, QObject* pParent = nullptr);
    ~CollabTransferService() override;

    /// Starts accepting transfer requests on `port` (zero picks a free port).
    bool listen(quint16 port, QString* pError = nullptr);
    void close();
    bool isListening() const;
    quint16 localPort() const;

    /// Rejects connections from any other address. An invalid address clears
    /// the restriction.
    void setAllowedPeer(const QHostAddress& address);

    /// Makes a local file available under its content hash.
    void serveFile(const CollabContentHash& hash, const QString& path);
    void removeServedFile(const CollabContentHash& hash);

    /// Starts receiving `hash` from `peer`. A partial file left by an earlier
    /// attempt is reused, so the transfer resumes. Returns false when the
    /// request cannot be started.
    bool fetch(const QHostAddress& peer,
            quint16 port,
            const CollabContentHash& hash,
            const QString& suffix,
            QString* pError = nullptr);
    void cancelFetch(const CollabContentHash& hash);
    bool isFetching(const CollabContentHash& hash) const;

  signals:
    void fetchProgress(
            const mixxx::collab::CollabContentHash& hash, qint64 received, qint64 total);
    void fetchFinished(const mixxx::collab::CollabContentHash& hash,
            const QString& path);
    void fetchFailed(const mixxx::collab::CollabContentHash& hash,
            const QString& error);
    void serveProgress(
            const mixxx::collab::CollabContentHash& hash, qint64 sent, qint64 total);
    void serveFinished(const mixxx::collab::CollabContentHash& hash);
    void serveFailed(const mixxx::collab::CollabContentHash& hash,
            const QString& error);

  private:
    struct Fetch;
    struct Serve;

    void onNewConnection();
    void onFetchReadyRead(QTcpSocket* pSocket);
    void onFetchDisconnected(QTcpSocket* pSocket);
    void failFetch(Fetch* pFetch, const QString& error);
    void finishFetch(Fetch* pFetch);
    void writeFetchedData(Fetch* pFetch, const QByteArray& data);

    void onServeReadyRead(QTcpSocket* pSocket);
    void pumpServe(Serve* pServe);
    void finishServe(Serve* pServe);
    void failServe(Serve* pServe, const QString& error);

    static QString hashKey(const CollabContentHash& hash);

    CollabTrackCache* m_pCache;
    QTcpServer* m_pServer;
    QHostAddress m_allowedPeer;
    QHash<QString, QString> m_servedFiles;
    std::map<QTcpSocket*, std::unique_ptr<Fetch>> m_fetches;
    std::map<QTcpSocket*, std::unique_ptr<Serve>> m_serves;
};

} // namespace collab
} // namespace mixxx
