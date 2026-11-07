#include "collab/collabdiagnostics.h"

#include "collab/clocksync.h"

namespace mixxx {
namespace collab {

namespace {

constexpr char kHeader[] =
        "kind,local_time_us,remote_time_us,"
        "t1_us,t2_us,t3_us,t4_us,rtt_us,offset_us,jitter_us,skew_ppm,"
        "command_id,target_us,actual_us,lateness_us,dropped,jump_us,"
        "pending_commands\n";

} // namespace

CollabDiagnostics::~CollabDiagnostics() {
    close();
}

bool CollabDiagnostics::open(const QString& path, QString* pError) {
    close();
    if (path.isEmpty()) {
        if (pError != nullptr) {
            *pError = QStringLiteral("empty diagnostics path");
        }
        return false;
    }

    m_file.setFileName(path);
    const bool existed = m_file.exists();
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (pError != nullptr) {
            *pError = m_file.errorString();
        }
        return false;
    }

    m_stream.setDevice(&m_file);
    m_headerWritten = existed && m_file.size() > 0;
    writeHeaderIfNeeded();
    return true;
}

void CollabDiagnostics::close() {
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
    m_stream.setDevice(nullptr);
    m_headerWritten = false;
}

void CollabDiagnostics::writeHeaderIfNeeded() {
    if (m_headerWritten || !m_file.isOpen()) {
        return;
    }
    m_stream << kHeader;
    m_stream.flush();
    m_headerWritten = true;
}

void CollabDiagnostics::logClockSample(qint64 localTimeUs,
        qint64 remoteTimeUs,
        const CollabClockSample& sample,
        qint64 offsetEstimateUs,
        qint64 jitterEstimateUs,
        double skewPpm) {
    if (!m_file.isOpen()) {
        return;
    }
    writeHeaderIfNeeded();
    // kind, local, remote, t1, t2, t3, t4, rtt, offset, jitter, skew,
    // (command fields empty), jump, pending
    m_stream << "clock," << localTimeUs << ',' << remoteTimeUs << ','
             << sample.t1Us << ',' << sample.t2Us << ',' << sample.t3Us << ','
             << sample.t4Us << ',' << sample.rttUs << ',' << offsetEstimateUs
             << ',' << jitterEstimateUs << ','
             << QString::number(skewPpm, 'f', 3) << ",,,,,,0,0\n";
    m_stream.flush();
}

void CollabDiagnostics::logScheduledCommand(quint64 commandId,
        qint64 targetSessionUs,
        qint64 actualSessionUs,
        qint64 latenessUs,
        bool dropped) {
    if (!m_file.isOpen()) {
        return;
    }
    writeHeaderIfNeeded();
    // kind, (clock fields empty), command_id, target, actual, lateness,
    // dropped, jump, pending
    m_stream << "command,,,,,,,,,,," << commandId << ',' << targetSessionUs
             << ',' << actualSessionUs << ',' << latenessUs << ','
             << (dropped ? 1 : 0) << ",0,0\n";
    m_stream.flush();
}

void CollabDiagnostics::logClockJump(
        qint64 localTimeUs, qint64 jumpUs, int pendingCommands) {
    if (!m_file.isOpen()) {
        return;
    }
    writeHeaderIfNeeded();
    // kind, local, remote, t1, t2, t3, t4, rtt, offset, jitter, skew,
    // command_id, target, actual, lateness, dropped, jump, pending
    // ("4000" + 14 empty fields + jump + pending = 18 columns)
    m_stream << "clockjump," << localTimeUs << ",,,,,,,,,,,,,,,"
             << jumpUs << ',' << pendingCommands << '\n';
    m_stream.flush();
}

} // namespace collab
} // namespace mixxx
