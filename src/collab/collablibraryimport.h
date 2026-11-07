#pragma once

#include "track/track.h"

class Library;

namespace mixxx {
namespace collab {

/// The crate received and shared tracks are collected in. Not translated, so
/// both peers see the same name.
constexpr char kCollabCrateName[] = "Collab";

/// Makes sure a crate named `Collab` exists and adds the track to it.
/// Existing membership is kept (the storage layer ignores duplicates).
/// Returns false when the library is unavailable or the database operation
/// failed; the track load itself is never affected.
bool importTrackIntoCollabCrate(
        Library* pLibrary, const TrackPointer& pTrack);

} // namespace collab
} // namespace mixxx
