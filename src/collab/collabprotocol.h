#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>

#include "collab/collabtypes.h"
#include "collab/trackhash.h"

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

    TrackOffer = 0x40,
    TrackHave = 0x41,
    TrackNeed = 0x42,
    LoadRequest = 0x43,
    LoadReady = 0x44,
    LoadFailed = 0x45,

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

/// One absolute control value inside a delta or a keyframe.
struct CollabControlValue {
    CollabControlId id = kInvalidCollabControlId;
    double value = 0.0;
};

/// One or more control changes. Sent at most 60 times per second.
struct CollabControlDelta {
    uint32_t deltaSeq = 0;
    uint32_t keyframeEpoch = 0;
    QList<CollabControlValue> values;
};

/// Full state of every mapped control. A snapshot larger than one datagram is
/// split into chunks that share an epoch and cover the map in order.
struct CollabControlKeyframe {
    uint32_t keyframeEpoch = 0;
    /// Deltas with a sequence at or below this one are obsolete.
    uint32_t deltaSeq = 0;
    /// Zero-based index of this chunk and the total number of chunks.
    uint16_t chunkIndex = 0;
    uint16_t chunkCount = 1;
    uint64_t sessionTimeUs = 0;
    QList<CollabControlValue> values;
};

/// Upper bound that keeps a keyframe inside one datagram.
constexpr int kCollabMaxControlsPerPacket = 96;

/// Owner playhead anchor for one deck. Positions are normalized to the track
/// duration so peers with different engine sample rates compare the same
/// musical position. Sent every second and immediately on transport changes.
struct CollabDeckAnchor {
    uint16_t deckId = 0;
    /// Increases with every anchor from this owner; older ones are discarded.
    uint32_t stateVersion = 0;
    /// Track length in seconds, zero when the deck has no track.
    double durationSeconds = 0.0;
    /// Playhead in [0, 1].
    double position = 0.0;
    double rate = 1.0;
    bool playing = false;
    bool loopEnabled = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    uint64_t anchorSessionTimeUs = 0;
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

/// One track the sender can serve.
struct CollabTrackRef {
    CollabContentHash hash;
    uint64_t size = 0;
    /// File extension without the dot, for example `flac`.
    QString suffix;
    QString title;
    /// The sender's TCP bulk port, so the receiver can pull the file.
    uint16_t transferPort = 0;
};

/// Asks the peer to load a track on its local replica of `deckId`. The sender
/// owns the deck and has the file; the receiver must end up with the same audio
/// before the deck may play on both peers.
struct CollabLoadRequest {
    uint16_t deckId = 0;
    CollabTrackRef track;
    /// Wait for waveform and beat grid before reporting ready.
    bool requireAnalysis = false;
};

/// The receiver has no copy of the offered hash and needs a transfer.
struct CollabTrackNeed {
    CollabContentHash hash;
};

enum class CollabLoadStatus : uint8_t {
    Ready = 0,
    Failed = 1,
};

/// Result of a `CollabLoadRequest`, carried by `LoadReady` or `LoadFailed`.
struct CollabLoadReply {
    uint16_t deckId = 0;
    CollabContentHash hash;
    CollabLoadStatus status = CollabLoadStatus::Ready;
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

QByteArray encodeCollabDeckAnchor(const CollabDeckAnchor& anchor);
bool decodeCollabDeckAnchor(
        const QByteArray& payload, CollabDeckAnchor* pAnchor);

QByteArray encodeCollabControlDelta(const CollabControlDelta& delta);
bool decodeCollabControlDelta(const QByteArray& payload, CollabControlDelta* pDelta);

QByteArray encodeCollabControlKeyframe(const CollabControlKeyframe& keyframe);
bool decodeCollabControlKeyframe(
        const QByteArray& payload, CollabControlKeyframe* pKeyframe);

QByteArray encodeCollabCommand(const CollabCommand& command);
bool decodeCollabCommand(const QByteArray& payload, CollabCommand* pCommand);

QByteArray encodeCollabCommandAck(const CollabCommandAck& ack);
bool decodeCollabCommandAck(const QByteArray& payload, CollabCommandAck* pAck);

QByteArray encodeCollabLoadRequest(const CollabLoadRequest& request);
bool decodeCollabLoadRequest(
        const QByteArray& payload, CollabLoadRequest* pRequest);

QByteArray encodeCollabTrackNeed(const CollabTrackNeed& need);
bool decodeCollabTrackNeed(const QByteArray& payload, CollabTrackNeed* pNeed);

QByteArray encodeCollabLoadReply(const CollabLoadReply& reply);
bool decodeCollabLoadReply(const QByteArray& payload, CollabLoadReply* pReply);

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
