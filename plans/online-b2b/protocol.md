# Protocol — Online B2B Collaboration

> **AI-generated plan.** This document was written autonomously by an AI agent in collaboration with the user. Review, validate, and adapt it before implementation.

This document specifies the proposed wire protocol. It is intentionally implementation-oriented: field layouts, message types, reliability rules, timing rules, and transfer semantics. It is not frozen; M0 may adjust constants.

## 1. Conventions

- Byte order: little-endian.
- Integers: fixed-width unsigned unless stated otherwise.
- Time: 64-bit microseconds (`uint64`), monotonic domain.
- Strings: `uint16` length + UTF-8 bytes, unless a fixed-size hash/ID is specified.
- Hash: 32-byte SHA-256.
- Session ID: 16 random bytes.
- Peer ID: 8 random bytes.
- Command ID: 8 bytes, unique per sender per session.
- Transfer ID: 8 bytes, unique per sender per session.
- Maximum UDP payload: 1200 bytes (IPv4/IPv6 safe, avoids fragmentation).
- Unreliable messages may be duplicated, delayed, reordered, or lost. Reliable messages are retransmitted until acknowledged.

## 2. Session identity and roles

```
sessionId   : 16 bytes, random at host session start
sessionEpoch: uint32, incremented on host restart/rejoin
hostPeerId  : 8 bytes
guestPeerId : 8 bytes
role        : HOST (owns session decks 1,2) or GUEST (owns 3,4)
sessionToken: 16 bytes, shared out-of-band; authenticates HELLO
```

A session is between exactly two peers in the MVP. The protocol includes peer IDs so it can be extended later, but no multi-peer routing is implemented.

## 3. UDP header

Every UDP datagram starts with this header:

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `MXXC` |
| 4 | 1 | `version` | Protocol version. Current value: 2 |
| 5 | 1 | `type` | Message type |
| 6 | 2 | `flags` | Bit 0: reliable; bit 1: request ACK; bit 2: response |
| 8 | 16 | `sessionId` | Must match the session |
| 24 | 8 | `senderPeerId` | Sender |
| 32 | 4 | `seq` | Packet sequence, per sender, wraps |
| 36 | 4 | `ack` | Highest packet sequence received |
| 40 | 8 | `ackBits` | Bitmap of the 64 packets before `ack` |
| 48 | 8 | `sendTimeUs` | Sender monotonic time |
| 56 | … | payload | Up to 1144 bytes |

The header is 56 bytes. The ACK fields refer to packet sequence numbers and are used by the reliable layer. Unreliable messages are still acknowledged at the packet level for RTT/loss measurement, but they are never retransmitted.

The receiver also estimates loss from gaps in `seq`. A 64-packet sliding window tolerates reordering: a packet that arrives late but still inside the window is no longer counted as lost. A gap is counted as soon as it appears, so a peer that goes silent after one late datagram still reports the loss. Handshake retries consume sequence numbers that the other side never accepts, so the window resets when the session reaches `Connected`. The counters feed the diagnostics UI only.

## 4. Message types

| ID | Name | Channel | Purpose |
|---:|---|---|---|
| 0x01 | `HELLO` | Reliable | Join request: session ID, peer ID, role, token, protocol range, feature flags. |
| 0x02 | `WELCOME` | Reliable | Accept: session epoch, host time, peer roles, deck ownership. |
| 0x03 | `REJECT` | Reliable | Join rejected: reason code, message. |
| 0x04 | `BYE` | Reliable | Graceful leave. |
| 0x10 | `CLOCK_PING` | Unreliable | t1. |
| 0x11 | `CLOCK_PONG` | Unreliable | t1, t2, t3. |
| 0x20 | `STATE_SNAPSHOT` | Reliable | Full peer/deck/control snapshot (small; large blobs on TCP). |
| 0x21 | `CONTROL_DELTA` | Unreliable-sequenced | One or more control changes. |
| 0x22 | `CONTROL_KEYFRAME` | Reliable | Full control keyframe with epoch. |
| 0x23 | `DECK_ANCHOR` | Unreliable-sequenced | Owner transport anchor. |
| 0x24 | `DECK_STATE` | Reliable | Loaded track, loop state, analysis availability. |
| 0x30 | `COMMAND` | Reliable | Transport/loop/hotcue command with target time. |
| 0x31 | `COMMAND_ACK` | Reliable | Applied/late/rejected. |
| 0x32 | `COMMAND_NACK` | Reliable | Rejected with reason. |
| 0x40 | `TRACK_OFFER` | Reliable | Offer one track. |
| 0x41 | `TRACK_HAVE` | Reliable | Receiver already has the hash. |
| 0x42 | `TRACK_NEED` | Reliable | Receiver needs the track. |
| 0x43 | `LOAD_REQUEST` | Reliable | Ask follower to load a deck. |
| 0x44 | `LOAD_READY` | Reliable | Follower loaded and verified. |
| 0x45 | `LOAD_FAILED` | Reliable | Load failed with reason. |
| 0x50 | `OWNERSHIP_REQUEST` | Reliable | Request deck takeover (post-MVP). |
| 0x51 | `OWNERSHIP_GRANT` | Reliable | Grant takeover (post-MVP). |
| 0x52 | `OWNERSHIP_RELEASE` | Reliable | Release ownership (post-MVP). |
| 0x60 | `KEEPALIVE` | Unreliable | Empty heartbeat. |
| 0x61 | `KEEPALIVE_ACK` | Unreliable | Heartbeat response. |
| 0x7F | `ERROR` | Reliable | Protocol/session error. |

Large payloads (state snapshot with many tracks, analysis blobs, crate offers) are sent over the TCP bulk channel, not as oversized UDP datagrams.

### Payload layouts

The subset implemented in `src/collab/collabprotocol.cpp` uses these exact
layouts. All integers are little-endian, `string` is a `uint16` byte length
followed by UTF-8 bytes (truncated to 512 bytes), and `float64` is IEEE-754
double. The stream uses a fixed `QDataStream` version, so the encoding does not
depend on the Qt version of either peer.

```
HELLO
  uint8  role              // 0 host, 1 guest
  uint8  reserved
  uint16 reserved
  uint32 featureFlags
  uint8  token[16]

WELCOME
  uint32 sessionEpoch
  uint8  role              // role granted to the receiver
  uint8  reserved
  uint16 reserved
  uint64 hostSessionTimeUs // initial anchor before the burst completes

BYE
  uint16 reasonCode
  string message

CLOCK_PING
  uint64 t1

CLOCK_PONG
  uint64 t1
  uint64 t2
  uint64 t3

COMMAND
  uint64 commandId
  uint8  action
  uint8  flags
  uint16 deckId
  uint64 targetSessionTimeUs
  uint32 argCount          // at most 4
  repeat argCount:
    uint16 argId
    float64 value
```

## 5. Channels and reliability

### Unreliable-sequenced

Used for:

- Continuous control deltas.
- Deck anchors.
- Clock pings/pongs.
- Keepalives.

Rules:

- The receiver keeps only the newest state per `(sender, deck/control epoch)`.
- Stale deltas are discarded by sequence/epoch.
- Loss is repaired by the next delta or the periodic keyframe/anchor.
- No retransmission.

### Reliable-ordered

Used for:

- Commands and ACKs.
- Track offers/load requests.
- Ownership changes.
- Session state snapshots.

Rules:

- Packet-level ACK uses `ack` + `ackBits`.
- Sender retransmits unacknowledged reliable packets.
- RTO: `srtt + 4 * rttvar`, clamped to `[20 ms, 250 ms]`.
- Fast retransmit after 3 duplicate ACKs.
- Maximum 5 retries, then declare the peer disconnected.
- The receiver delivers reliable messages in order. A hold-back queue of at most 64 messages; beyond that, request a snapshot instead of growing without bound.
- Commands are idempotent by `commandId`; duplicates are ACKed but not applied twice.

### Why not reliable UDP for bulk

Bulk transfer needs congestion control, pacing, resume, and verification. TCP already provides ordered delivery and flow control; using a separate TCP connection isolates its head-of-line blocking from the tiny control channel. A custom reliable-UDP bulk layer is a later option only if measurements show TCP is unsuitable.

## 6. Control replication

### Control IDs

The control set is negotiated at join. The host assigns a session-local 16-bit `controlId` to each `ConfigKey` (`[Group], item`) in sorted order and sends the complete `controlId → group/item` table. No hashing is used; hashes of Qt types are process-seeded and would not match across peers. If a duplicate or unknown ID occurs, the session fails with a clear error rather than silently mapping the wrong control.

The replicated set is defined in `architecture.md` section 6. Do not replicate UI-only controls, `playposition` (use anchors instead), or local monitoring controls.

### `CONTROL_DELTA`

```
uint32 deltaSeq
uint32 keyframeEpoch
uint16 count
repeat count:
  uint16 controlId
  float64 value
```

- Sent at most 60 Hz per peer.
- Bundled with other deltas into one datagram. A burst that exceeds one
  datagram is split into chunks of at most 96 controls, each with its own
  increasing `deltaSeq`.
- Deadband per control type: for example, `1e-4` for normalized faders/EQ, `1e-3` BPM for tempo, exact for buttons.
- Values are absolute, not additive. A lost delta is repaired by the next absolute value.

### `CONTROL_KEYFRAME`

```
uint32 keyframeEpoch
uint32 deltaSeq                 // deltas with seq <= this are obsolete
uint16 chunkIndex               // zero-based
uint16 chunkCount               // 1 when the snapshot fits in one datagram
uint64 sessionTimeUs
uint16 count
repeat count:
  uint16 controlId
  float64 value
```

- Sent every 5 s, immediately on start, and once more 250 ms after start in
  case the peer was still starting its replicator when the first one arrived.
- A keyframe only carries the controls this peer is authoritative for: its own
  decks, the host's crossfader, and shared groups such as the standard effect
  units. Both peers send keyframes, so overlapping authority would make the
  snapshots overwrite each other every interval.
- A snapshot larger than one datagram is split into at most 96 controls per
  chunk. The chunks share an epoch and carry increasing `deltaSeq` values so
  the receiver does not discard the later chunks as stale.
- A lost chunk leaves a partial snapshot until the next keyframe; the chunks
  are not reassembled.
- The receiver discards older deltas.
- The M1 implementation keeps a fixed epoch for the life of a replicator run,
  so a lost periodic keyframe does not invalidate the deltas that follow. A
  reconnect restarts the replicator and advances the epoch, so the peer that
  stayed connected accepts the fresh keyframe instead of treating its restarted
  delta sequence as stale. A peer that sees a higher epoch sends its own
  keyframe immediately, so both sides resynchronise without waiting for the
  periodic snapshot.
- The M1 control map is derived on both peers from the same session-space key
  list (`collabDefaultControlKeys`). The map message itself is not exchanged
  yet; both peers must run the same commit.

### Echo suppression

The network layer uses one `ControlProxy` per control. `ControlProxy::set` marks the proxy as sender; its own `valueChanged` is suppressed. A short value/time guard handles engine confirmations. Do not use static `ControlObject::set` for remote application.

## 7. Clock synchronization

### Exchange

```
CLOCK_PING:
  uint64 t1

CLOCK_PONG:
  uint64 t1
  uint64 t2   // receive time at responder
  uint64 t3   // send time at responder
```

Sender records `t4` on receive.

```
offset = ((t2 - t1) + (t3 - t4)) / 2
rtt    = (t4 - t1) - (t3 - t2)
```

- All times are in the same monotonic unit (microseconds).
- Burst 8 exchanges on join, spaced ~50 ms.
- Steady state: 1 Hz.
- Keep the last 32 valid samples.
- Discard samples with `rtt > 2 * minRtt`.
- Offset estimate: median of the remaining samples.
- Skew estimate: linear regression of offset vs sample time; use only if the residual is below a threshold and at least 8 samples are available.
- Re-burst if `|offset - previousOffset| > 10 ms` or `minRtt` changes by more than 50%.

### Session time

- Host: `sessionTime = hostMonotonic - hostEpochMonotonic`.
- Guest: `sessionTime = guestMonotonic + offsetToSession`.
- The WELCOME message includes the host's current session time, used as the initial anchor before the burst completes.
- Scheduled commands are disabled until the clock is considered stable (at least 4 valid samples and a bounded residual).

### Timestamp quality

MVP uses userspace timestamps. Later, kernel receive timestamps improve accuracy. NTP-style clock sync aligns CPU clocks; audio-device drift is corrected by anchors, not by clock sync.

## 8. Scheduling and commands

### `COMMAND`

```
uint64 commandId
uint8  action
uint8  flags              // bit 0: immediate; bit 1: quantized to beat; bit 2: quantized to bar
uint16 deckId
uint64 targetSessionTimeUs
uint32 argCount
repeat argCount:
  uint16 argId
  float64 value
```

Actions (MVP):

| Action | Args | Notes |
|---|---|---|
| `PLAY` | — | Start/resume. |
| `PAUSE` | — | Pause. |
| `SEEK` | `positionFrames` | Track frames. |
| `CUE_START` | — | Return to main cue and pause/play per flags. |
| `HOTCUE_ACTIVATE` | `hotcueIndex` | Trigger. |
| `LOOP_SET` | `startFrames`, `endFrames`, `enabled` | Set and optionally enable. |
| `LOOP_ENABLE` | `enabled` | Toggle. |
| `BEATLOOP` | `beats` | Beatloop length. |
| `RATE_SET` | `rate` | Tempo ratio. |
| `KEYLOCK_SET` | `enabled` | Keylock. |
| `TRACK_LOAD` | `hash` (32 bytes) | Large metadata comes via `LOAD_REQUEST`/TCP. |

### Scheduling rules

- The sender computes `target = nowSession + lead`.
- `lead = clamp(max(2 * RTT, 3 * jitter, 40 ms), 40 ms, 100 ms)`. Default 60 ms.
- The receiver places the command in a min-heap keyed by target time.
- At the target, apply the command. If the target has already passed, apply the late policy and return `COMMAND_ACK(late)`.
- The owner also schedules its own local action at the target in scheduled mode. This is the only way to make both sides land together.
- Immediate mode sets `target = now` and applies on receipt. The owner applies locally immediately.
- Quantized mode computes the next beat/bar target from the owner's beatgrid and sends that target. The follower does not need to run quantize.

### `COMMAND_ACK`

```
uint64 commandId
uint8  result          // applied, late, rejected, not_ready, duplicate
uint64 appliedSessionTimeUs
uint16 reasonCode
string reason
```

### Late policy

| Action | Late behavior |
|---|---|
| `PLAY` | Compute the position the track would have at `now`; seek there and play. If the track has not started, start at the computed position. |
| `PAUSE` | Pause immediately. |
| `SEEK` | Seek immediately. |
| `CUE_START` | Apply immediately; next anchor corrects. |
| `HOTCUE_ACTIVATE` | Apply immediately; next anchor corrects. |
| `LOOP_*` | Apply immediately; loop points are exact. |
| `RATE_SET` | Apply immediately. |
| `KEYLOCK_SET` | Apply immediately. |

## 9. Deck anchors

### `DECK_ANCHOR`

Implemented in M1. Wire layout in `collabprotocol.h`:

```
uint16 deckId
uint32 stateVersion
float64 durationSeconds
float64 position            // playhead normalized to [0, 1]
float64 rate
uint8  flags                // bit 0 playing, bit 1 loop enabled
uint8  reserved
float64 loopStart           // normalized to [0, 1]
float64 loopEnd             // normalized to [0, 1]
uint64 anchorSessionTimeUs
```

Positions are normalized to the track duration, not sent as frames, so the
two peers may run their engines at different sample rates. The track hash and
sample rate from the original design are not in the M1 packet yet; the follower
compares the local track duration instead and ignores anchors that differ by
more than 1% or 0.5 s.

- Sent every 1 s while playing.
- Sent immediately on play, pause, seek, rate change, loop change and reconnect.
- Paused decks send on state change only, and when accumulated playhead
  movement crosses the scratch threshold.
- Playhead deviation accumulates across `playposition` updates. A scratch
  moves the playhead only a few milliseconds per audio buffer, so no single
  update crosses the threshold; the sum is compared instead. Scratch and nudge
  anchors are throttled to one every 25 ms.
- `stateVersion` increases on every owner anchor and never resets across a
  reconnect, so a follower that stayed connected does not discard the fresh
  anchors as stale. The follower discards older anchors.

### Expected position

```
if (!playing):
    expected = position
else if (!loopEnabled):
    expected = position + rate * (nowSession - anchorSessionTimeUs) / 1e6 / durationSeconds
else:
    raw = position + rate * (nowSession - anchorSessionTimeUs) / 1e6 / durationSeconds
    expected = loopStart + positiveModulo(raw - loopStart, loopEnd - loopStart)
```

`nowSession` is the follower's session time, converted from its local clock
through the clock estimate. Both `position` and the loop bounds are fractions
of the track duration.

### Correction thresholds

Implemented in M1: the follower hard-seeks when the absolute error reaches
5 ms. Rate nudging (the 5–50 ms row below) is not implemented yet.

| Error | Action |
|---|---|
| < 5 ms | Ignore. |
| 5–50 ms | Rate nudge ±0.1% to ±0.5% until corrected, then restore the owner's rate. |
| > 50 ms | Hard seek with crossfade or a short mute; paused decks seek silently. |

Do not continuously resample. Do not correct the owner's deck.

## 10. Track offer and load

### `TRACK_OFFER`

```
uint8  hash[32]
uint64 fileSize
string fileName
string format
float64 durationSeconds
uint32 sampleRate
uint16 channels
uint8  hasWaveform
uint8  hasWaveSummary
uint8  hasBeats
uint16 cueCount
string waveformVersion
string waveformSummaryVersion
string beatsVersion
string beatsSubVersion
```

### `TRACK_NEED`

Implemented layout:

```
uint8 hash[32]
```

The receiver sends it after a `LOAD_REQUEST` whose hash is neither in the cache
nor in the local hash index. It then pulls the file from the `transferPort`
carried by the request.

The earlier crate-batch variant adds `uint8 source` (library, cache, incoming);
it is not implemented yet.

### `LOAD_REQUEST`

Implemented layout:

```
uint16 deckId
uint8  hash[32]
uint64 fileSize
uint16 transferPort     // sender's TCP bulk server
uint8  flags            // bit 0: requireAnalysis
string suffix           // UTF-8, uint16 length
string title            // UTF-8, uint16 length
```

### `LOAD_READY` / `LOAD_FAILED`

Implemented layout:

```
uint16 deckId
uint8  hash[32]
uint8  status           // 0 ready, 1 failed
uint16 reasonCode
string reason           // UTF-8, uint16 length
```

### Two-phase commit

1. Owner sends `LOAD_REQUEST` with the hash, metadata and its TCP port.
2. Follower checks its library/cache by hash.
3. If present: load, attach analysis, reply `LOAD_READY`.
4. If missing: reply `TRACK_NEED`; the follower pulls the file from the request's
   `transferPort`, verifies the full SHA-256, installs it in the cache, loads it
   and replies `LOAD_READY`.
5. The owner may load locally immediately but must gate play until `LOAD_READY`.
6. Timeout/cancel produces `LOAD_FAILED` and a UI reason.

## 11. TCP bulk channel

### Implemented framing (M2 first slice)

Each peer runs a server. The receiver opens one TCP connection per track and
sends one request. The sender answers with one fixed header and then streams the
file from the requested offset. The receiver writes into `<hash>.part`, verifies
the full SHA-256 after the last byte, and only then renames the file into the
content-addressed cache.

Request, 46 bytes, big-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `magic` = ASCII `MXXT` |
| 4 | 2 | `version` = 1 |
| 6 | 32 | `sha256` of the whole file |
| 38 | 8 | `offset` already present in the partial file |

Response, 17 bytes plus an optional message, big-endian:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `magic` = ASCII `MXXT` |
| 4 | 2 | `version` = 1 |
| 6 | 1 | `status`: 0 ok, 1 not found, 2 bad request, 3 bad offset |
| 7 | 8 | total file size |
| 15 | 2 | `messageLength` |
| 17 | … | UTF-8 error message |

After an `ok` response the sender streams the raw file bytes starting at
`offset`. The receiver treats the remaining bytes as file content; no chunk
headers are used. Resume is therefore a single durable offset: the size of the
partial file. A partial file that is already corrupt is detected at the final
hash check, deleted, and the next attempt starts from zero.

Tuning: 32 KiB read chunks, a 512 KiB send window driven by `bytesToWrite()`,
a 15 s inactivity timeout, and at most four concurrent fetches.

### Planned extension

The frame set below adds chunk acknowledgements, a durable bitmap, transfer
priority and compression. It is not implemented yet; the request/response above
is enough to move files and resume at byte granularity.

| ID | Name | Purpose |
|---:|---|---|
| 0x01 | `HELLO` | Session/peer authentication, protocol version. |
| 0x02 | `OFFER` | Transfer offer: hash, size, name, format, chunk size. |
| 0x03 | `ACCEPT` | Accept with `startOffset` and durable bitmap/offset. |
| 0x04 | `REJECT` | Reason (disk space, hash mismatch, busy, etc.). |
| 0x10 | `CHUNK` | `transferId`, `offset`, `length`, data. |
| 0x11 | `ACK` | `transferId`, `offset`, `length`, durable bitmap/offset. |
| 0x12 | `CANCEL` | Cancel; keep or delete temp. |
| 0x13 | `COMPLETE` | Final hash, size. |
| 0x14 | `ERROR` | Error code and message. |

### Transfer semantics

- Chunk size: 64 KB default, 256 KB maximum.
- The receiver writes to `<hash>.part` and maintains a bitmap in `<hash>.part.meta`.
- The sender sends a window of chunks (for example, 8–32). The receiver ACKs durable chunks.
- On reconnect, the receiver sends `ACCEPT` with the durable offset/bitmap; the sender resumes at the first missing chunk.
- The receiver verifies the full SHA-256 after all chunks. On mismatch, delete the temp file and send `ERROR`.
- On success, atomically rename the temp file into the content-addressed cache and update the hash index.
- Compression: if the source is WAV/AIFF and FLAC encoding is available, the `OFFER` includes `transferFormat=flac` and the original hash. The receiver verifies the original hash after decode.
- Pacing: token bucket at 50–70% of the configured uplink; small socket send buffer; respect `bytesToWrite()`; optional `SO_MAX_PACING_RATE` on Linux.
- Priority: current load > next track > crate bulk. Pause bulk while a load is waiting.
- Preflight: reject if free disk space is insufficient; sanitize filenames; never write outside the Collab cache/library folder.

## 12. Security

### MVP

- Tailscale/WireGuard provides link encryption and NAT traversal.
- `HELLO` includes a 16-byte session token shared out-of-band. The host compares it in constant time and rejects mismatches.
- Session ID + epoch + sequence window prevent accidental cross-session mixing.
- File transfers are accepted only from the paired peer.
- No library enumeration: only offered hashes/metadata are sent.

### Later

- Authenticated encryption for UDP (Noise/QUIC/DTLS) and TLS/Noise for TCP.
- Rendezvous service and relay for NAT-restricted peers.
- Rate limiting and replay protection across sessions.
- Do not hand-roll cryptography.

### Control map negotiation

Control identifiers are assigned by the host after `HELLO`/`WELCOME`:

```
CONTROL_MAP:
  uint8  mapVersion
  uint16 count
  repeat count:
    uint16 controlId          // 1..N, never 0
    string group
    string item
```

- Identifiers are session-local and are assigned in sorted `ConfigKey` order.
- Both peers must validate the map before applying any `CONTROL_DELTA`.
- An unknown `controlId`, a duplicate id/key, a malformed string, or a missing
  local `ControlProxy` rejects the session with a clear error. The receiver
  must never guess the meaning of an unknown control.
- A new session renegotiates the map. Do not persist control identifiers.

### Fragmentation

- The UDP payload limit is 1200 bytes. `STATE_SNAPSHOT`, `CONTROL_KEYFRAME`,
  and any other reliable message that can exceed this limit are fragmented.
- Fragment header: `uint32 messageId`, `uint16 fragmentIndex`,
  `uint16 fragmentCount`, `uint16 fragmentLength`.
- Reassembly is bounded: at most 64 KiB per message, at most 4 messages in
  flight, and a 5 s timeout. Missing fragments are retransmitted by the
  reliable layer.
- Messages larger than 64 KiB (analysis blobs, large crate offers) use the TCP
  bulk channel instead.

### Timestamp domains

- `CLOCK_PING.t1` and `CLOCK_PONG.t2/t3` are sender-local monotonic
  microseconds.
- The host defines session time. The guest maps its local monotonic clock to
  session time with the estimated offset.
- Define `offset = hostSessionTime - guestLocalTime`. Test the sign convention
  explicitly, including a negative offset.

## 13. Versioning and errors

- `version` is the protocol major version. Minor features are advertised in `HELLO` feature flags.
- Unknown message types are ignored (with a debug log), not fatal.
- Unknown required feature flags cause `REJECT` with a clear message.
- Error codes:

| Code | Meaning |
|---:|---|
| 1 | Bad version |
| 2 | Bad session |
| 3 | Bad token |
| 4 | Session full |
| 5 | Unsupported feature |
| 6 | Clock not synchronized |
| 7 | Track not available |
| 8 | Transfer failed |
| 9 | Hash mismatch |
| 10 | Disk full |
| 11 | Deck busy |
| 12 | Not owner |
| 13 | Timeout |
| 14 | Protocol error |

## 14. Example flows

### Join

```
B -> A: HELLO(sessionId, peerId, role=GUEST, token, version, features)
A -> B: WELCOME(sessionEpoch, hostSessionTime, roles, ownership)
B -> A: CLOCK_PING x8
A -> B: CLOCK_PONG x8
B -> A: STATE_SNAPSHOT (B's deck state, controls, tracks)
A -> B: STATE_SNAPSHOT (A's deck state, controls, tracks)
A -> B: CONTROL_KEYFRAME
B -> A: CONTROL_KEYFRAME
```

### Session lifecycle

Implemented by `CollabSession` (`src/collab/collabsession.cpp`):

| State | Meaning |
|---|---|
| `Idle` | No socket bound. |
| `Listening` | Host is bound and waits for one guest. |
| `Joining` | Guest bound its port and repeats `HELLO` until `WELCOME`. |
| `Connected` | Both peers agree on session id, peer ids and endpoints. |
| `Closed` | Terminal failure: bad token, handshake timeout, or an explicit `BYE`. A lost peer is retried instead when auto-reconnect is on. |

Rules:

- The guest sends `HELLO` with a zero session id; the host answers with the
  session id it generated. Later `HELLO`s on a retry may carry that id.
- The host compares the 16-byte token in constant time. A mismatch is answered
  with `REJECT` carrying a zero session id, so an unauthenticated sender learns
  nothing about the session.
- A `HELLO` that names a different session is rejected with `REJECT` echoing the
  sender's session id. A second endpoint that presents the right token while the
  host is already connected is rejected as busy.
- A joining guest accepts `WELCOME` and `REJECT` only from the host address and
  port it joined; other senders are ignored.
- Connected packets must match the session id, the peer id and the peer
  endpoint. Anything else is dropped.
- `KEEPALIVE` every 500 ms; a peer silent for 2500 ms is declared gone.
  Without auto-reconnect the session moves to `Closed` with an error. With
  auto-reconnect the socket stays bound: the host returns to `Listening` and
  keeps the session id and remote endpoint, so a guest that never noticed the
  drop is revived by its next keepalive; the guest returns to `Joining` and
  repeats `HELLO`. The local audio keeps running and the remote decks are
  marked stale until the session is restored. Replication restarts on the
  `Connected` transition, which sends a fresh keyframe and anchors.
- `CLOCK_PING` every 200 ms while connected (configurable). Briefly after the
  handshake the exchange doubles as the join burst.
- `BYE` (or closing the session) tells the other peer to stop with an error
  that names the reason. The local side returns to `Idle`.

### Scheduled play

```
A: user presses play on deck 1
A: target = now + 60 ms
A: schedule local PLAY at target
A -> B: COMMAND(PLAY, deck=1, target)
B: schedule PLAY at target
A/B: apply at target
B -> A: COMMAND_ACK(applied, actualTime)
```

### Load with transfer

```
A -> B: LOAD_REQUEST(deck=3, hash)
B -> A: TRACK_NEED(hash)
A -> B: TCP OFFER(hash, size, format)
B -> A: TCP ACCEPT(startOffset=0)
A -> B: TCP CHUNK...
B -> A: TCP ACK...
A -> B: TCP COMPLETE(hash)
B: verify hash, install, import, attach analysis
B -> A: LOAD_READY(deck=3, hash)
A: enable play
```

### Fader update

```
A: volume change 0.72
A -> B: CONTROL_DELTA(controlId=volume(deck1), 0.72)
B: apply to local [Channel1] volume
```

## 15. Bandwidth estimates

| Traffic | Estimate |
|---|---|
| Control deltas | 4 decks × ~8 controls × 5 bytes × 60 Hz ≈ 10 kbit/s, less with deadband |
| Keyframes | ~1–4 kbit/s average |
| Anchors | ~1 kbit/s |
| Clock | < 1 kbit/s |
| Total control | well under 50 kbit/s per peer |
| Bulk | capped at 50–70% of uplink, separate TCP connection |

The control channel must remain small and prioritized. Bulk transfer must never share the control socket.

## 16. Limits and assumptions

- Two peers only in the MVP.
- One owner per deck; fixed ownership.
- Both peers must run compatible protocol versions.
- Both peers must have working local audio devices.
- Track identity is exact SHA-256; different rips/encodings are different tracks.
- Effects are not replicated; remote decks should have effects bypassed in the MVP.
- Cross-peer beat sync is disabled; each deck's owner computes rate/phase.
- The session clock is monotonic; wall-clock changes do not affect scheduling.
- The protocol assumes a trusted peer on a private network for the MVP.

> **AI-generated plan.** This document was written autonomously by an AI agent. Review and validate before implementation.
