#pragma once

#include <QFile>
#include <QString>
#include <QTextStream>
#include <QtGlobal>

namespace mixxx {
namespace collab {

struct CollabClockSample;

/// CSV logging for the M0 diagnostics harness.
///
/// The file is append-only so a run can be interrupted without losing the
/// samples collected so far. One header is written when the file is created.
class CollabDiagnostics {
  public:
    CollabDiagnostics() = default;
    ~CollabDiagnostics();

    CollabDiagnostics(const CollabDiagnostics&) = delete;
    CollabDiagnostics& operator=(const CollabDiagnostics&) = delete;

    /// Opens or creates the CSV file for appending. Returns false and sets
    /// `pError` when the file cannot be opened.
    bool open(const QString& path, QString* pError = nullptr);
    void close();
    bool isOpen() const {
        return m_file.isOpen();
    }
    QString path() const {
        return m_file.fileName();
    }

    /// One completed clock exchange and the resulting estimate.
    void logClockSample(qint64 localTimeUs,
            qint64 remoteTimeUs,
            const CollabClockSample& sample,
            qint64 offsetEstimateUs,
            qint64 jitterEstimateUs,
            double skewPpm);

    /// One scheduled command and its actual execution time.
    void logScheduledCommand(quint64 commandId,
            qint64 targetSessionUs,
            qint64 actualSessionUs,
            qint64 latenessUs,
            bool dropped);

  private:
    void writeHeaderIfNeeded();

    QFile m_file;
    QTextStream m_stream;
    bool m_headerWritten = false;
};

} // namespace collab
} // namespace mixxx
