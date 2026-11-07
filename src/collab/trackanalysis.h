#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "track/beats.h"
#include "track/track.h"
#include "waveform/waveform.h"

class QColor;

namespace mixxx {
namespace collab {

class CollabContentHash;

/// One serialized analysis result, for example a waveform, its summary or a
/// beat grid. `version` and `subVersion` are the strings Mixxx uses in its
/// analysis database, so an entry round-trips through the same code paths.
struct CollabTrackAnalysisEntry {
    enum class Type : uint8_t {
        Waveform = 0,
        WaveformSummary = 1,
        Beats = 2,
    };

    Type type = Type::Waveform;
    QString version;
    QString subVersion;
    QByteArray data;
};

/// The analysis results that travel with a shared track. Applied to the
/// receiver's `Track` so the peer sees the same waveform and beat grid without
/// re-analyzing. Version mismatches are handled by Mixxx's own analysis code:
/// an entry the local Mixxx cannot read is dropped and local analysis fills
/// the gap.
struct CollabTrackAnalysis {
    /// Sample rate the beats were detected at, needed to deserialize them.
    quint32 sampleRate = 0;
    QVector<CollabTrackAnalysisEntry> entries;

    bool isEmpty() const {
        return entries.isEmpty();
    }
};

/// Serializes the analysis results of `pTrack` (waveform, waveform summary,
/// beats) into a bundle. Returns an empty array when the track has nothing
/// worth sending.
QByteArray encodeCollabTrackAnalysisBundle(const CollabTrackAnalysis& analysis);

/// Parses a bundle produced by `encodeCollabTrackAnalysisBundle`. Returns
/// false on truncation, size overflow or an unknown bundle version.
bool decodeCollabTrackAnalysisBundle(
        const QByteArray& blob, CollabTrackAnalysis* pAnalysis);

/// Builds the analysis bundle from a locally loaded track.
CollabTrackAnalysis buildCollabTrackAnalysis(const TrackPointer& pTrack);

/// Applies a received bundle to `pTrack`. Entries the local Mixxx cannot
/// read (unknown versions) are skipped; that keeps the load working and
/// local analysis fills the gap.
void applyCollabTrackAnalysis(
        const TrackPointer& pTrack, const CollabTrackAnalysis& analysis);

} // namespace collab
} // namespace mixxx
