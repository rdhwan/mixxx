#pragma once

#include <QString>

#include "collab/collabtypes.h"

namespace mixxx {
namespace collab {

/// Immutable mapping between session deck identifiers and local deck groups.
///
/// Session decks are numbered 1..4. The host owns 1 and 2, the guest owns 3
/// and 4. The host maps session deck N to local ChannelN. The guest mirrors:
/// its own session decks 3 and 4 show as local Channel1 and Channel2 (where
/// its skin, keyboard and controller act), and the host's decks 1 and 2 show
/// as local Channel3 and Channel4. Parity is preserved, so a session deck
/// keeps its crossfader side on both peers.
class CollabDeckMap {
  public:
    explicit CollabDeckMap(CollabRole role);

    CollabRole role() const {
        return m_role;
    }

    bool ownsSessionDeck(int sessionDeckId) const;

    /// Returns the local one-based deck index for a session deck, or
    /// kInvalidCollabDeckId when the session deck is invalid.
    int localDeckForSessionDeck(int sessionDeckId) const;

    /// Returns the session deck for a local one-based deck index, or
    /// kInvalidCollabDeckId when the local deck is invalid.
    int sessionDeckForLocalDeck(int localDeck) const;

    /// Crossfader orientation for a session deck. Odd session decks are left,
    /// even session decks are right. Returns -1 for an invalid deck.
    ///
    /// The values match EngineChannel::ChannelOrientation (LEFT = 0,
    /// RIGHT = 2) but the engine header is intentionally not included here so
    /// the mapping stays unit-testable without the engine.
    int crossfaderOrientationForSessionDeck(int sessionDeckId) const;

    /// Local Mixxx group, e.g. "[Channel3]".
    QString localGroupForSessionDeck(int sessionDeckId) const;

    /// Local zero-based deck index used by PlayerManager-like APIs.
    int localDeckIndexForSessionDeck(int sessionDeckId) const;

  private:
    CollabRole m_role;
};

} // namespace collab
} // namespace mixxx
