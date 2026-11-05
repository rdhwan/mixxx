#pragma once

#include <QString>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mixxx {
namespace collab {

/// Random identifier of a peer within a session.
using CollabPeerId = std::array<uint8_t, 8>;

/// Random identifier of a collaboration session.
using CollabSessionId = std::array<uint8_t, 16>;

/// Session-local identifier of a replicated control.
///
/// The identifier is assigned by the host at join time and is only valid for
/// the duration of a session. It is deliberately not derived from a hash of the
/// ConfigKey because process-local hash seeds would make it unstable.
using CollabControlId = uint16_t;

constexpr CollabControlId kInvalidCollabControlId = 0;

/// The session deck identifiers used on the wire are 1..4.
constexpr int kCollabSessionDeckCount = 4;
constexpr int kInvalidCollabDeckId = 0;

/// Wire protocol version.
constexpr uint8_t kCollabProtocolVersion = 1;

/// Largest UDP payload that is safe for both IPv4 and IPv6 without IP
/// fragmentation.
constexpr int kCollabMaxDatagramSize = 1200;

enum class CollabRole {
    Host,
    Guest,
};

constexpr bool isValidCollabDeckId(int deckId) {
    return deckId >= 1 && deckId <= kCollabSessionDeckCount;
}

QString collabRoleToString(CollabRole role);

/// Returns the role that owns the two other session decks.
constexpr CollabRole otherCollabRole(CollabRole role) {
    return role == CollabRole::Host ? CollabRole::Guest : CollabRole::Host;
}

QString peerIdToHex(const CollabPeerId& peerId);
QString sessionIdToHex(const CollabSessionId& sessionId);

bool peerIdFromHex(const QString& hex, CollabPeerId* pPeerId);
bool sessionIdFromHex(const QString& hex, CollabSessionId* pSessionId);

} // namespace collab
} // namespace mixxx
