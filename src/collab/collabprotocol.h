#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>

#include "collab/collabtypes.h"

namespace mixxx {
namespace collab {

/// Size of the fixed UDP header that starts every datagram.
constexpr int kCollabHeaderSize = 56;

/// Message types. The values are wire constants; do not renumber them.
enum class CollabMessageType : uint8_t {
    Hello = 0x01,
    Welcome = 0x02,
    Reject = 0x03,
    Bye = 0x04,

    ClockPing = 0x10,
    ClockPong = 0x11,

    ControlDelta = 0x21,
    ControlKeyframe = 0x22,
    DeckAnchor = 0x23,
    DeckState = 0x24,

    Command = 0x30,
    CommandAck = 0x31,
    CommandNack = 0x32,

    Keepalive = 0x60,
    KeepaliveAck = 0x61,
    Error = 0x7F,
};

/// Header flags. The reliable layer retransmits flagged packets.
enum CollabPacketFlag : uint16_t {
    CollabFlagNone = 0,
    CollabFlagReliable = 1 << 0,
    CollabFlagRequestAck = 1 << 1,
    CollabFlagResponse = 1 << 2,
};

/// Commands carried by `CollabMessageType::Command`. Values are wire
/// constants; append new actions, never renumber existing ones.
enum class CollabCommandAction : uint8_t {
    Play = 1,
    Pause = 2,
    Seek = 3,
    CueStart = 4,
    HotcueActivate = 5,
    LoopSet = 6,
    LoopEnable = 7,
    Beatloop = 8,
    RateSet = 9,
    KeylockSet = 10,
};

/// Result reported back in `CommandAck`.
enum class CollabCommandResult : uint8_t {
    Applied = 0,
    Late = 1,
    Rejected = 2,
    NotReady = 3,
    Duplicate = 4,
};

/// Argument identifiers used by `CollabCommandAction`. They are interpreted per
/// action; one action may use the same identifier with a different meaning
/// only if the protocol document says so.
enum CollabCommandArgId : uint16_t {
    CollabArgPositionFrames = 1,
    CollabArgHotcueIndex = 2,
    CollabArgLoopStartFrames = 3,
    CollabArgLoopEndFrames = 4,
    CollabArgEnabled = 5,
    CollabArgBeats = 6,
    CollabArgRate = 7,
};

constexpr int kCollabMaxCommandArgs = 4;

struct CollabHeader {
    CollabMessageType type = CollabMessageType::Keepalive;
    uint16_t flags = CollabFlagNone;
    CollabSessionId sessionId{};
    CollabPeerId senderPeerId{};
    uint32_t seq = 0;
    /// Highest sequence number received from the other peer.
    uint32_t ack = 0;
    /// Bitmap of the 64 packets before `ack`; bit 0 is `ack - 1`.
    uint64_t ackBits = 0;
    /// Sender's monotonic time, used for RTT and loss measurement.
    uint64_t sendTimeUs = 0;
};

/// One decoded datagram: a fixed header plus an opaque payload.
struct CollabPacket {
    CollabHeader header;
    QByteArray payload;
};

/// Writes the 56-byte header followed by `payload`.
QByteArray encodeCollabPacket(const CollabHeader& header, const QByteArray& payload);

/// Parses a datagram. Returns false for foreign traffic, an unknown version, a
/// truncated header, or a datagram larger than the safe UDP payload.
bool decodeCollabPacket(const QByteArray& data, CollabPacket* pPacket);

QString collabMessageTypeToString(CollabMessageType type);
QString collabCommandActionToString(CollabCommandAction action);

/// Session join request. The session id travels in the header.
struct CollabHello {
    CollabRole role = CollabRole::Host;
    /// Shared secret, 16 bytes, compared with a constant-time equality.
    std::array<uint8_t, 16> token{};
    uint32_t featureFlags = 0;
};

struct CollabWelcome {
    uint32_t sessionEpoch = 0;
    /// Role granted to the receiver.
    CollabRole role = CollabRole::Guest;
    /// Host session time when the welcome was built, an initial clock anchor.
    uint64_t hostSessionTimeUs = 0;
};

struct CollabBye {
    uint16_t reasonCode = 0;
    QString message;
};

struct CollabClockPing {
    uint64_t t1Us = 0;
};

struct CollabClockPong {
    uint64_t t1Us = 0;
    uint64_t t2Us = 0;
    uint64_t t3Us = 0;
};

struct CollabCommandArg {
    uint16_t id = 0;
    double value = 0.0;
};

struct CollabCommand {
    uint64_t commandId = 0;
    CollabCommandAction action = CollabCommandAction::Play;
    /// Bit 0: immediate, bit 1: quantized to beat, bit 2: quantized to bar.
    uint8_t flags = 0;
    uint16_t deckId = 0;
    uint64_t targetSessionTimeUs = 0;
    std::array<CollabCommandArg, kCollabMaxCommandArgs> args{};
    uint8_t argCount = 0;

    bool addArg(uint16_t id, double value);
};

struct CollabCommandAck {
    uint64_t commandId = 0;
    CollabCommandResult result = CollabCommandResult::Applied;
    uint64_t appliedSessionTimeUs = 0;
    uint16_t reasonCode = 0;
    QString reason;
};

QByteArray encodeCollabHello(const CollabHello& hello);
bool decodeCollabHello(const QByteArray& payload, CollabHello* pHello);

QByteArray encodeCollabWelcome(const CollabWelcome& welcome);
bool decodeCollabWelcome(const QByteArray& payload, CollabWelcome* pWelcome);

QByteArray encodeCollabBye(const CollabBye& bye);
bool decodeCollabBye(const QByteArray& payload, CollabBye* pBye);

QByteArray encodeCollabClockPing(const CollabClockPing& ping);
bool decodeCollabClockPing(const QByteArray& payload, CollabClockPing* pPing);

QByteArray encodeCollabClockPong(const CollabClockPong& pong);
bool decodeCollabClockPong(const QByteArray& payload, CollabClockPong* pPong);

QByteArray encodeCollabCommand(const CollabCommand& command);
bool decodeCollabCommand(const QByteArray& payload, CollabCommand* pCommand);

QByteArray encodeCollabCommandAck(const CollabCommandAck& ack);
bool decodeCollabCommandAck(const QByteArray& payload, CollabCommandAck* pAck);

/// Constant-time comparison for the 16-byte session token.
bool collabTokensEqual(
        const std::array<uint8_t, 16>& lhs,
        const std::array<uint8_t, 16>& rhs);

/// Builds a randomized session id from a platform CSPRNG.
CollabSessionId makeCollabSessionId();
/// Builds a randomized peer id from a platform CSPRNG.
CollabPeerId makeCollabPeerId();
/// Builds a randomized session token from a platform CSPRNG.
CollabSessionToken makeCollabSessionToken();

/// Derives the session token from a short code the two peers exchange out of
/// band. The mapping is one way, case insensitive and tolerant of surrounding
/// whitespace, so a code read aloud still matches.
CollabSessionToken collabTokenFromCode(const QString& code);

/// Generates a short session code without visually ambiguous characters.
QString makeCollabSessionCode();

} // namespace collab
} // namespace mixxx
