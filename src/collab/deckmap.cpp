#include "collab/deckmap.h"

#include "collab/collabtypes.h"

namespace mixxx {
namespace collab {

namespace {
constexpr int kLeftCrossfaderOrientation = 0;
constexpr int kRightCrossfaderOrientation = 2;
} // namespace

CollabDeckMap::CollabDeckMap(CollabRole role)
        : m_role(role) {
}

bool CollabDeckMap::ownsSessionDeck(int sessionDeckId) const {
    if (!isValidCollabDeckId(sessionDeckId)) {
        return false;
    }
    return m_role == CollabRole::Host
            ? sessionDeckId <= 2
            : sessionDeckId >= 3;
}

int CollabDeckMap::localDeckForSessionDeck(int sessionDeckId) const {
    if (!isValidCollabDeckId(sessionDeckId)) {
        return kInvalidCollabDeckId;
    }
    return sessionDeckId;
}

int CollabDeckMap::sessionDeckForLocalDeck(int localDeck) const {
    if (!isValidCollabDeckId(localDeck)) {
        return kInvalidCollabDeckId;
    }
    return localDeck;
}

int CollabDeckMap::crossfaderOrientationForSessionDeck(int sessionDeckId) const {
    if (!isValidCollabDeckId(sessionDeckId)) {
        return -1;
    }
    return (sessionDeckId % 2) == 0 ? kRightCrossfaderOrientation
                                    : kLeftCrossfaderOrientation;
}

QString CollabDeckMap::localGroupForSessionDeck(int sessionDeckId) const {
    const int localDeck = localDeckForSessionDeck(sessionDeckId);
    if (localDeck == kInvalidCollabDeckId) {
        return QString();
    }
    return QStringLiteral("[Channel%1]").arg(localDeck);
}

int CollabDeckMap::localDeckIndexForSessionDeck(int sessionDeckId) const {
    const int localDeck = localDeckForSessionDeck(sessionDeckId);
    if (localDeck == kInvalidCollabDeckId) {
        return -1;
    }
    return localDeck - 1;
}

} // namespace collab
} // namespace mixxx
