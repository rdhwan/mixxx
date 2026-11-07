#pragma once

#include <QString>

#include "collab/collabtypes.h"

namespace mixxx {
namespace collab {

/// Immutable mapping between session deck identifiers and local deck groups.
///
/// Session decks are numbered 1..4. The host owns 1 and 2, the guest owns 3
/// and 4. Both peers use the same layout: session deck N is local ChannelN on
/// both machines. A peer's own decks are therefore Channel1/2 on the host and
/// Channel3/4 on the guest.
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
