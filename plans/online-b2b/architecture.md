# Architecture — Online B2B Collaboration

> **AI-generated plan.** This document was written autonomously by an AI agent in collaboration with the user. Review, validate, and adapt it before implementation.

## 1. Decision summary

| Area | Decision |
|---|---|
| Playback | Replicated deck-shadow. Both instances run the full four-deck engine and play local files. |
| Authority | Each deck has exactly one owner. The owner is authoritative for that deck's audio-affecting state. |
| Control transport | UDP. Unreliable-sequenced for continuous controls, reliable-ordered for commands. |
| Bulk transport | Separate TCP side channel for track files and large analysis blobs. |
| Clock | NTP-style four-timestamp exchange, min-RTT/median filter, skew regression, shared session time. |
| Timing | Scheduled target-time commands with a 40–100 ms adaptive lead, plus an immediate mode. |
| Mixer | Host owns the shared crossfader. Each peer owns their own channel strip. PFL and master/booth levels are local. |
| Sharing | Opt-in Collab crate/playlist, content-addressed cache, prefetch, resumable transfer, full hash verification, load gating. |
| Connectivity | Tailscale/WireGuard first. Rendezvous/relay and authenticated encryption are later. |

The architecture is deliberately **not** host-authoritative. In the host-authoritative model the guest hears the host's engine output, which adds a return-audio path and 100–200 ms of latency. The replicated model removes that path entirely. The cost is drift correction and track-file matching, which this plan addresses directly.

## 2. System context

```
+-----------------------------+                         +-----------------------------+
| Mixxx A (host)              |                         | Mixxx B (guest)             |
|                             |                         |                             |
|  [Channel1] own deck 1      |   UDP control plane     |  [Channel1] own deck 3      |
|  [Channel2] own deck 2      |<----------------------->|  [Channel2] own deck 4      |
|  [Channel3] replica deck 3  |   clock / commands      |  [Channel3] replica deck 1  |
|  [Channel4] replica deck 4  |   anchors / deltas      |  [Channel4] replica deck 2  |
|                             |                         |                             |
|  local audio device         |   TCP bulk channel      |  local audio device         |
|  local files                |<----------------------->|  local files / cache        |
|  CollabManager              |   tracks / analysis     |  CollabManager              |
+-----------------------------+                         +-----------------------------+
```

Both machines produce the full four-deck mix locally. Each peer hears their own machine. The network has no audio in the playback path.

### Same-room caveat

If both machines feed loudspeakers in the same room, the two copies of the same mix will comb-filter and echo. Only one machine should feed the PA; the other should be headphones-only or muted. Remote B2B is the primary case.

## 3. Components

Proposed new module: `src/collab/`.

| Component | Thread | Responsibility |
|---|---|---|
| `CollabManager` | Main/UI | Session lifecycle, dialog, preferences, track-load orchestration, library import, peer status. |
| `CollabSession` | Network | UDP socket, peer table, packet routing, reliability, keepalive, session epoch. |
| `CollabProtocol` | Network | Binary encode/decode, versioning, message validation. |
| `ClockSync` | Network | Four-timestamp exchange, offset/RTT/jitter/skew estimation, session-time mapping. |
| `CommandScheduler` | Scheduler | Target-time min-heap, high-resolution wait, apply or dispatch commands. |
| `ControlReplicator` | Network/Scheduler | Map session control IDs to local `ControlProxy`s; observe local changes, apply remote values, echo suppression, keyframes. |
| `DeckMap` | Shared (immutable) | Session deck ID ↔ local group mapping, owner, orientation, labels. |
| `DeckStateReplicator` | Network | Transport anchors, deck state versions, keyframes, drift error reporting. |
| `TrackHashIndex` | Worker pool | Content hash cache, background hashing, path/size/mtime invalidation. |
| `TrackTransferService` | Network + workers | TCP server/client, chunking, resume, pacing, verification, atomic install. |
| `CollabLibrary` | Main | Import received files, save waveform/beats/cues, manage the Collab folder and cache. |
| `CollabUi` | Main/UI | Session dialog, deck badges, lock indicators, transfer progress, debug overlay. |

The module is opt-in. When disabled, no sockets are opened and no controls are connected.

## 4. Threading model

```
Audio callback thread
  └── EngineBuffer / EngineMixer (existing)
      └── No collab calls, no allocations, no locks.
          Optional future: lock-free scheduled-command queue for sample-accurate play/seek.

Network thread
  └── CollabSession
      ├── QUdpSocket (control)
      ├── QTcpServer / QTcpSocket (bulk)
      ├── ClockSync
      ├── ControlReplicator
      └── DeckStateReplicator
      └── Never blocks on disk, UI, or database.

Scheduler thread
  └── CommandScheduler
      └── High-resolution wait; applies transport commands or emits
          queued signals for main-thread work.

Main/UI thread
  └── CollabManager
      ├── CollabUi
      ├── Track load / PlayerManager / Library
      ├── CollabLibrary / TrackCollectionManager
      └── Preferences

Worker pool
  └── TrackHashIndex (hashing)
  └── TrackTransferService (FLAC encode/decode, file IO)
```

### Cross-thread rules

1. **Audio thread:** no collaboration code. The only possible future exception is a preallocated lock-free command queue for sample-accurate scheduling, and only after M0 proves it is needed.
2. **Network thread:** may read and write `ControlProxy` values. This is the same pattern used by controllers. It must not call library or database APIs.
3. **Main thread:** all track loads, library lookups, database writes, and analysis persistence.
4. **Scheduler thread:** applies transport controls (play/seek/loop) or emits a queued signal for a main-thread load. It must not block.
5. **Worker pool:** hashing, compression, decompression, and file IO. Results are delivered to the network/main thread by queued signals.

### Why applying controls from the network thread is acceptable

`ControlDoublePrivate` stores values atomically. `ControlProxy::set(value)` passes the proxy as the sender, so the proxy's own `valueChanged` signal is suppressed while other controls observe the change. Control-change-request handlers connected with `Qt::DirectConnection` run in the caller's thread; that is already how controller threads drive `play`, `seek`, and similar controls. The replication layer follows the same contract and restricts itself to controls that are safe to drive like a controller.

Controls that expect the main thread (track load, library selection, UI-only state) are **not** replicated as controls. They use explicit messages and queued signals.

## 5. Deck model and local mapping

### Session decks

Session deck IDs are `1..4`. They are independent of local `[ChannelN]` groups.

- Host (peer A) owns session decks **1, 2**.
- Guest (peer B) owns session decks **3, 4**.
- Ownership is fixed for the MVP. A takeover handshake is a later milestone.

### Local group mapping

On the host:

| Session deck | Local group |
|---|---|
| 1 | `[Channel1]` |
| 2 | `[Channel2]` |
| 3 | `[Channel3]` |
| 4 | `[Channel4]` |

On the guest:

| Session deck | Local group |
|---|---|
| 1 | `[Channel3]` |
| 2 | `[Channel4]` |
| 3 | `[Channel1]` |
| 4 | `[Channel2]` |

General formula for the local one-based deck index:

```
localDeck = ((sessionDeck - 1) + (isGuest ? 2 : 0)) % 4 + 1
```

This keeps each peer's physical controller on local `Channel1/2`, which are their own session decks. The friend's decks appear on local `Channel3/4`. No controller remapping is required.

### Crossfader orientation

Typical four-deck layout: odd session decks left, even session decks right.

| Session deck | Orientation |
|---|---|
| 1 | Left |
| 2 | Right |
| 3 | Left |
| 4 | Right |

Set `[ChannelN], orientation` consistently on both machines using the session deck ID, not the local group. This ensures the host's crossfader produces the same balance on both machines.

### UI labels

Each deck header shows a session label and ownership:

- `1 · YOU` / `1 · FRIEND`
- `2 · YOU` / `2 · FRIEND`
- etc.

A remote deck is visually locked. The lock is enforced by the replication layer in M1; a richer skin-level lock is later.

## 6. Ownership and control scope

### Ownership rules

- The owner is the only peer allowed to change the deck's audio-affecting state.
- The follower applies the owner's state and corrections. It does not run its own quantize/sync logic for that deck.
- Local input on a remote deck is ignored or reverted, with a visible "owned by friend" indication.
- A takeover request/grant flow is post-MVP. Until then, ownership is fixed per session.

### Control scope table

| Control group | Examples | Scope | Notes |
|---|---|---|---|
| Transport | `play`, `start_stop`, `playposition` seeks, `repeat` | Shared | Owner issues; follower mirrors. |
| Tempo/rate | `rate`, `tempo`, `rate_dir`, `keylock` | Shared | Follower applies the owner's resulting rate. |
| Loops | `loop_in`, `loop_out`, `loop_enabled`, `beatloop_*`, `loop_halve/double` | Shared | Owner computes quantized positions; follower mirrors. |
| Hotcues | `hotcue_N_activate`, `hotcue_N_set/clear` | Shared | Cue data travels with the track; triggering is a command. |
| Channel strip | `volume`, `pregain`, EQ/filter parameters and kills, `mute`, `orientation` | Shared | Each peer owns their own decks' strips. `orientation` is the deck's crossfader side and is replicated so both crossfaders balance the same. |
| Crossfader | `[Master], crossfader`, curve, mode, reverse | Shared, host-owned | Guest input is disabled or overridden. |
| PFL / headphones | `cue`, `headMix`, `headGain`, `headEnabled` | Local only | Personal monitoring. |
| Master / booth | `[Master], gain`, `booth_gain`, `balance`, `booth_enabled` | Local only | Each machine's listening level. |
| Output device | Audio device selection | Local only | Not a control. |
| Sync | `sync_enabled`, `sync_mode`, `sync_leader` | Owner-local | Cross-peer sync is disabled in MVP; replicas do not run sync. |
| Quantize | `quantize` | Owner-local | Follower applies the owner's resulting positions. |
| Effects | Standard units: `enabled`, `mix`, `super1`, `mix_mode`, deck routing, slot effect, slot `enabled`/`meta`, `parameter1..16`, `button_parameter1..16`; per-deck quick effect chain | Shared | Standard units are editable by both peers with the host as keyframe authority. A quick effect chain belongs to its deck owner. Loading applies before parameters, the chain super knob before slot meta values, and slot enables last. Track/effect packs and visible-effect order must match on both peers. Presets are not replicated. |
| Samplers / preview / mic / aux | — | Not replicated | Out of scope. |
| AutoDJ / recording / broadcast | — | Not replicated | Out of scope. |

### Why PFL is local

The PFL/cue button feeds the headphone mix, not the main mix. Each DJ needs independent headphone monitoring. Replicating PFL would force both peers to hear the same cue mix and would fight over the headphone bus. Keep it local.

### Why master gain is local

The main mix balance must match, but the final listening level is per-machine. If the host raises master gain, the guest should not be forced to raise their speakers. The shared mix is defined before the local master/booth gain stage.

### Crossfader limitation

Mixxx has one global crossfader. With a host-owned crossfader, the guest can transition with channel faders but cannot use their own crossfader. A deck-pair crossfader (host's crossfader affects decks 1/2, guest's affects 3/4) requires an engine change and is a later feature. The MVP documents the limitation.

## 7. State replication

### Per-deck owner state

Each deck's owner publishes:

- Track identity: `contentHash`, size, format, duration, sample rate, channels, metadata.
- Transport anchor: track position, rate, playing, loop state, session timestamp.
- Control keyframe: all replicated control values for that deck.
- Analysis availability: waveform, waveform summary, beats, cues.

### Continuous controls

- Sampling: up to 60 Hz, one bundled UDP datagram per tick.
- Deadband: per-control epsilon (for example, `1e-4` normalized for faders/EQ) to avoid sending noise.
- Encoding: control ID + value. Float64 for MVP; optional quantized integers later.
- Loss tolerance: unreliable-sequenced. A missing delta is repaired by the next delta or the 5 s keyframe.
- Ordering: per-sender delta sequence. Discard stale deltas after a newer keyframe epoch.

### Commands

- Reliable-ordered over UDP.
- Each command has a 64-bit `commandId`, a target session time, a deck ID, an action, and arguments.
- Idempotent: duplicate `commandId` is acknowledged but not applied twice.
- ACK contains the command ID and result (`applied`, `rejected`, `late`, `not-ready`).
- If the follower rejects a command (for example, deck not loaded), the owner receives a NACK and surfaces it in the UI.

### Keyframes and snapshots

- Full control keyframe every 5 s per peer.
- Full deck/state snapshot on join and reconnect.
- Snapshot includes session epoch, peer roles, ownership, loaded track hashes, anchors, control values, and analysis availability.
- Large analysis blobs are sent over TCP, not in the UDP snapshot.

### Echo suppression

`ControlProxy::set` marks the proxy as the sender. The proxy's own `valueChanged` is suppressed, so a remote-applied value is not immediately echoed. For controls that are confirmed by the engine (for example, `play`), a short value/time guard suppresses the confirmation rebroadcast if it matches the last remote-applied value. A new local user change is always sent.

Do not use the static `ControlObject::set(key, value)` for remote application: it passes `nullptr` as the sender and is indistinguishable from a local change.

### Conflict handling

- Owner input wins.
- Follower input on a remote deck is ignored/reverted in M1.
- Takeover is a future explicit request/grant, not last-writer-wins.

## 8. Clock sync and scheduling

### Session clock

- The host defines the session epoch and a monotonic session clock.
- Session time is a 64-bit microsecond counter derived from `std::chrono::steady_clock` (or an equivalent monotonic source).
- The guest maps its local monotonic time to session time using the estimated offset.

### Four-timestamp exchange

```
A (host)                       B (guest)
   | t1  CLOCK_PING  -----------> |
   |                              | t2 receive
   | <---------  CLOCK_PONG t1,t2,t3 | t3 send
   | t4 receive                    |
```

```
offset = ((t2 - t1) + (t3 - t4)) / 2
rtt    = (t4 - t1) - (t3 - t2)
```

- Burst of 8 exchanges on join.
- Steady state: 1 Hz.
- Keep the last 32 samples. Discard samples with RTT > 2 × minimum RTT. Use the median offset among the remaining samples.
- Estimate skew with a linear regression of offset over time; use it only if the residual is stable.
- Re-burst if the offset jumps by more than 10 ms or the minimum RTT changes significantly.

### Timestamp quality

- MVP: userspace timestamps taken as close to the socket call as possible.
- Later: kernel receive timestamps (`SO_TIMESTAMPNS` on Linux, `WSARecvMsg` on Windows) for sub-millisecond accuracy.
- NTP syncs CPU clocks, not audio device clocks. Audio drift is handled separately by anchors.

### Scheduling

- Commands carry `targetSessionTimeUs`.
- The sender computes `target = now + lead`.
- `lead = clamp(max(2 × RTT, 3 × jitter, 40 ms), 40 ms, 100 ms)`. Default 60 ms.
- The scheduler applies the command at the target time on both machines.
- Scheduled mode intentionally delays the owner's own action by the lead so both sides land together. This is a control-action delay, not an audio-path delay.
- Immediate mode applies the owner's action immediately and sends the command for best-effort application; the next anchor corrects the follower.

### Audio-buffer quantization

The engine reads `play` and processes seeks at buffer boundaries. At 44.1 kHz:

| Buffer | Duration |
|---|---|
| 128 frames | 2.9 ms |
| 256 frames | 5.8 ms |
| 512 frames | 11.6 ms |

Target-time scheduling can align to the nearest buffer boundary but cannot start mid-buffer without engine changes. M0 measures whether this is good enough. If not, add a preallocated lock-free scheduled-command queue that the audio callback drains for play/seek/loop, applying a command at a frame offset within the buffer.

### Late policy

| Command | Late behavior |
|---|---|
| Play | Compute the position the track would be at now; seek there and play, so the deck stays musically aligned. If the track has not started, start at the computed position. |
| Cue / hotcue | Apply immediately; the next anchor corrects. If quantized mode is requested, apply at the next beat instead. |
| Loop | Apply immediately; loop points are exact. |
| Seek | Apply immediately; the next anchor corrects. |
| Continuous control | Apply immediately; no target time. |

### Quantized mode

For musical actions, the sender can request "apply at the next beat" or "apply at the next bar". The owner computes the target session time from its beatgrid and sends that target. The follower does not need the beatgrid to apply the command; it just honors the target time. This avoids running two different quantize implementations.

## 9. Drift correction

### Why it is needed

NTP aligns CPU clocks. It does not align the two audio devices. A 50 ppm difference accumulates 180 ms per hour. If the follower's replica of the owner's deck drifts against the follower's local decks, the follower's mix goes out of phase.

### Anchor message

The owner sends a deck anchor every 1–2 s while playing, immediately on play, seek, rate change, loop change, or track load, and whenever the playhead deviates from its expected path by more than 20 ms in total. The deviation accumulates across `playposition` updates because a scratch moves the playhead only a few milliseconds per audio buffer; comparing single updates would never trigger. Scratch and nudge anchors are spaced at least 25 ms apart. Anchor fields:

```
deckId
trackHash
trackSampleRate
positionFrames      // track frames, not engine frames
rate
playing
loopEnabled
loopStartFrames
loopEndFrames
anchorSessionTimeUs
stateVersion
```

Position is expressed in track frames (or track seconds) so it is independent of each machine's engine sample rate. Both machines may run 44.1 kHz or 48 kHz and resample the same file; track time is the common domain.

### Follower correction

Given an anchor, the follower computes the expected position at local session time `T`:

```
expected = position + rate * (T - anchorTime) * trackSampleRate
```

with loop wrap if looping. It compares `expected` to the replica's actual track position.

| Error | Action |
|---|---|
| < 5 ms | Ignore. Do not micro-adjust. |
| 5–50 ms | Adjust the replica's rate by a small amount (±0.1% to ±0.5%) until the error is corrected, then restore the owner's rate. |
| > 50 ms | Hard seek with a short crossfade, or mute for a few milliseconds around the seek. Paused decks seek silently. |

Do not continuously resample the replica. Do not correct the owner's deck. Corrections apply only to replicas.

### Long-session behavior

Because anchors arrive every 1–2 s, the accumulated drift between anchors is well under 1 ms even for a 100 ppm clock difference. The main error source is clock-offset/jitter measurement, not drift. The 5 ms threshold keeps corrections rare and inaudible.

## 10. Track identity and sharing

### Content identity

- Full-file SHA-256 plus file size. SHA-256 is available through `QCryptographicHash` and is already used in the tree.
- BLAKE3 is a later optimization if hashing throughput becomes a bottleneck; it adds a dependency.
- Exact hash match is required for sample-accurate playback. Fuzzy metadata matching is a later convenience feature and must warn that the audio may differ.

### Hash index

- Sidecar SQLite database in the Collab config directory (Qt Sql/SQLite is already available).
- Tables: `file_hash(path, size, mtime, hash, algo, updated)` and `cache(hash, path, size, format, state, last_used)`.
- Invalidate on size/mtime change.
- Hash on demand for selection/prefetch/load.
- Optional low-priority background scan of the library, resumable across restarts.
- Hashing runs on a worker pool and streams the file in blocks; never blocks the UI or audio thread.

### Content-addressed cache

- `~/.mixxx/collab/cache/<sha256>/<sanitized-name>` plus a metadata sidecar.
- Deduplicate by hash. If the peer's library already has the same hash, link to that file instead of copying.
- LRU cleanup with a user-configurable size cap. Never delete files referenced by the library without an explicit warning.

### Offer / need / have

1. Sender offers a track: hash, size, name, format, duration, sample rate, channels, metadata, analysis availability.
2. Receiver replies `HAVE` if the hash exists in its library or cache, otherwise `NEED`.
3. For a crate/playlist push, the sender batches offers; the receiver replies with the list of needed hashes.
4. If `NEED`, the sender starts (or queues) a TCP transfer.

### Prefetch triggers

- Track selection in the library (`Library::trackSelected` is emitted on selection).
- Next track in the playlist/AutoDJ queue.
- Collab crate/playlist selection or explicit "Share with peer".
- Manual "Send to peer".

Default policy: only the opt-in Collab crate/playlist is auto-prefetched. The whole library is not enumerated. A visible indicator shows what is being shared.

### Load is a two-phase commit

```
Owner                         Follower
  | LOAD_REQUEST(hash, meta) ------> |
  |                                  | have hash? load : request transfer
  | <------ LOAD_READY / NEED -------|
  | (transfer if needed)             |
  | <------ LOAD_READY --------------|  after full hash + analysis
  | play allowed                     |
```

- The owner may load the track locally immediately, but the play command is gated until the follower reports `LOAD_READY`.
- The follower loads only after the file is fully received, hash-verified, and analysis is available or explicitly deferred.
- If the transfer fails or times out, the owner sees the reason and can cancel, retry, or choose another track.
- This is the price of no audio streaming. Prefetch is the mitigation.

### Transfer

- Separate TCP connection, separate port from UDP control.
- Chunk size 64 KB (configurable up to 256 KB). Receiver maintains a durable bitmap in a `.part.meta` file.
- On reconnect, the receiver reports the bitmap/durable offset; the sender resumes at the first missing chunk.
- Write to a temporary `.part` file. After full hash verification, atomically rename into the content-addressed cache.
- Full SHA-256 verification before the track becomes loadable. A mismatch deletes the temp file and reports an error.
- Optional compression: MP3/FLAC/OGG are sent as-is; WAV/AIFF are encoded to FLAC in transit. The receiver verifies the original hash after decode. If FLAC encoding is unavailable or too slow, send raw.
- Pacing: token bucket at 50–70% of the measured/configured uplink; small socket send buffer; respect `bytesToWrite()` backpressure; optional `SO_MAX_PACING_RATE` on Linux; bulk traffic is lower priority than control.
- One transfer queue with priorities: current load > next track > crate bulk. Pause bulk when a load is waiting.
- Disk-space check before accepting. Sanitize the peer-provided filename; never trust a path.
- Security: Tailscale encrypts the link in the MVP. Direct-internet TLS/Noise is a later milestone.

### Analysis transfer

Mixxx can serialize the important analysis data:

- `Waveform::toByteArray()` / `Waveform(const QByteArray&)`.
- `Beats::toByteArray()` / `Beats::fromByteArray(sampleRate, version, subVersion, bytes)`.
- Cues need a small manual serializer (type, start/end ms, hotcue index, label, color, flags).
- `AnalysisDao::saveAnalysis` can persist waveform/summary blobs for an imported track.

Flow:

1. Send metadata and waveform/beat/cue analysis over TCP before or alongside the audio.
2. The receiver shows an "incoming" row with metadata and waveform while the audio is still transferring.
3. After the file completes and is imported, save the analysis to the database and attach it to the track.
4. If the analysis version is incompatible, discard it and let Mixxx analyze locally. The track is still loadable after the audio hash verifies; the waveform may appear later.

The M2 spike must validate the database/analysis insertion path for an imported track. If it is too invasive, fall back to a custom incoming-track view and local analysis.

### Library integration

- Received files go to a configurable Collab folder (for example, `<music root>/Mixxx Collab/` or `~/.mixxx/collab/library/`).
- Import via `TrackCollectionManager::getOrAddTrack` on the main thread.
- Optionally add a "Collab" crate for received tracks.
- The original peer filename is preserved for display but sanitized for the filesystem.
- Cleanup is explicit; don't silently delete a track that is in the library.

## 11. Connectivity and security

### MVP: Tailscale/WireGuard

- Both peers join the same tailnet.
- Mixxx listens on a configurable UDP control port and TCP bulk port.
- Tailscale provides NAT traversal and link encryption.
- A session token in the handshake prevents accidental cross-session joins.
- Document firewall rules and the Tailscale requirement.

### Later: public internet

- Rendezvous service for peer discovery and session codes.
- STUN/TURN or a relay for NAT-restricted peers.
- Authenticated encryption for UDP (Noise/QUIC/DTLS) and TLS/Noise for TCP.
- Do not hand-roll cryptography. Use a reviewed library.
- Rate limiting, replay protection, and peer authentication.

### Threat model for MVP

- Trusted peer on a private tailnet.
- Session token prevents accidental joins, not a determined attacker on the same tailnet.
- File transfers are accepted only from the paired peer and only after hash verification.
- No remote code execution, no path traversal, no automatic library enumeration.

## 12. UI/UX

### Session dialog

- Start session / Join session.
- Session code or invite string.
- Peer list with connection state, RTT, jitter, clock status, and packet loss.
- Deck ownership summary: "You: 1, 2. Friend: 3, 4."
- "Share Collab crate" action.

### Deck UI

- Session number and owner badge on each deck header.
- Lock icon on remote decks.
- "Waiting for track", "Downloading 43%", "Verifying", "Ready" states.
- Crossfader indicator: "Controlled by host" on the guest.
- A small status strip with the peer connection.

### Transfer panel

- Per-track progress, speed, ETA, priority, cancel/retry.
- Queue view showing current load, next track, and bulk crate transfers.

### Debug overlay

- RTT, jitter, clock offset, skew, anchor error, state version, packet loss, late commands, buffer size, correction count, transfer rate.
- M1 shows RTT, offset, jitter, skew, clock samples, last remote drift, packet loss (from sequence gaps, reorder-tolerant), and dropped scheduled commands in both the legacy dialog and the QML settings page.
- Hidden behind a developer/debug menu or a command-line flag.

### Preferences

- Enable collaboration.
- Collab folder.
- Uplink cap / pacing percentage.
- Auto-share Collab crate on/off.
- Default scheduled lead (40–100 ms).
- Control/bulk ports.
- Tailscale setup help.

### Error states

- Peer disconnected: local audio continues; remote decks marked stale; a "mute remote decks" action is available. No automatic mute during a live set.
- Clock unsynchronized: scheduled commands disabled until the offset is stable; immediate mode still works.
- Track missing / transfer failed: deck gated with a clear reason.
- Hash mismatch: transfer rejected, temp file deleted, retry offered.
- Ownership conflict: local change reverted with a toast.
- Version mismatch: collaboration disabled with a clear message.

## 13. Failure handling

| Failure | Behavior |
|---|---|
| Peer disconnect | Local audio continues. Remote decks continue at their local rate and are marked stale. Anchors stop. On reconnect, full snapshot and re-anchor; hard seek if the error is large. |
| Packet loss | Continuous controls recover from deltas/keyframes. Commands retransmit and are deduplicated. Anchors are periodic. |
| Late command | Apply the action-specific late policy; report `late` in the ACK. |
| Clock jump | Re-burst; do not jump already-scheduled events; reschedule future events. |
| Track not ready | Owner play is gated; follower reports `not-ready`. |
| Transfer failure | Keep the `.part` for resume; report the error; retry with backoff. |
| Hash mismatch | Delete the temp file; never load; retry. |
| Disk full | Reject the transfer before writing; report free space. |
| Ownership conflict | Owner wins; follower input reverted; takeover later. |
| Protocol version mismatch | Refuse the session with a clear message. |
| Corrupt packet | Drop; log at debug level; never crash. |

## 14. Testing strategy

### Unit tests

- Protocol encode/decode, bounds checks, fuzz malformed packets.
- Clock offset/RTT/jitter/skew filters.
- Command scheduler ordering and late policy.
- Control map and deck mapping.
- Hash index invalidation and cache lookups.
- Chunk bitmap and resume logic.
- Echo suppression and keyframe epoch handling.

### Integration tests

- Two `CollabSession` instances in one process over loopback.
- Mocked control bus for deterministic assertions.
- Two Mixxx instances with separate settings paths for end-to-end tests where feasible.
- `tc netem` profiles: 20 ms RTT, 50 ms RTT + 20 ms jitter, 1–2% loss, duplication, reordering.
- Kill/restart a peer; kill the network; change the clock; fill the disk; corrupt a chunk.

### Manual tests

- Two real machines, two controllers, real audio interfaces.
- 30-minute and 2-hour sessions.
- Track load with and without the file on the peer.
- Transfer during active mixing; confirm no control latency spike or audio underrun.
- Same-room and remote scenarios.

### Metrics

- Control RTT/jitter/offset/skew.
- Anchor error and correction count.
- Late command count.
- Packet loss/retransmit rate.
- Transfer rate and resume count.
- `[App], audio_latency_overload_count` and `audio_latency_usage` from Mixxx.
- CPU and memory.

## 15. Proposed code layout

```
src/collab/
  collabmanager.{h,cpp}
  collabsession.{h,cpp}
  collabprotocol.{h,cpp}
  collabtypes.h
  clocksync.{h,cpp}
  commandscheduler.{h,cpp}
  controlreplicator.{h,cpp}
  deckmap.{h,cpp}
  deckstatereplicator.{h,cpp}
  trackhashindex.{h,cpp}
  tracktransfer.{h,cpp}
  collablibrary.{h,cpp}
  collabpreferences.{h,cpp}
  ui/
    dlcollab.{h,cpp,ui}
    collabstatuswidget.{h,cpp}
    collabdebugoverlay.{h,cpp}

src/test/collab/
  collabprotocol_test.cpp
  clocksync_test.cpp
  commandscheduler_test.cpp
  controlreplicator_test.cpp
  deckmap_test.cpp
  trackhashindex_test.cpp
  tracktransfer_test.cpp
  collabsession_test.cpp
```

`CMakeLists.txt` uses explicit source lists; add the new files and tests there. Qt Network and Qt Sql are already linked. No new dependency is required for M1/M2. FLAC transit uses the existing audio codec stack if available; otherwise raw transfer is the fallback.

## 16. Risks and open questions

### Risks

1. **Scheduling quantization.** M0 must measure the real alignment. If buffer-boundary scheduling is not good enough, the engine-side command queue becomes a prerequisite for M1, not M3.
2. **Audio clock drift.** Anchors should handle it; the two-hour soak must prove it.
3. **Track transfer latency.** A missing 100 MB WAV cannot be instant. The UX must make gating visible and prefetch must be aggressive.
4. **Analysis insertion.** Saving waveform/beats/cues for an imported track may touch database internals. Spike it early.
5. **Effects.** Standard units and per-deck quick effects replicate in M1, but `loaded_effect` carries an index into the visible-effect list: both peers must have the same effect packs and the same visible order. Chain presets are not replicated, and the two mixes still differ if a peer uses an effect the other lacks.
6. **Crossfader.** Host-owned is a compromise. Confirm the guest is comfortable using channel faders, or schedule the deck-pair crossfader earlier.
7. **NAT/security.** Tailscale is a dependency. Public-internet relay and crypto are a separate project.
8. **Upstream scope.** This is a large feature. Keep it modular and opt-in; upstream acceptance is a separate decision.

### Open questions

- Should the default scheduled lead be 60 ms or adaptive per session? (Plan: 60 ms default, adaptive 40–100 ms.)
- Should the guest be able to request crossfader control for a transition? (Later.)
- Should received tracks be auto-imported or kept in an "Incoming" list until accepted? (Plan: import after verification, with a Collab crate; revisit in M2.)
- Should analysis transfer be mandatory before `LOAD_READY`, or can the deck load with local analysis? (Plan: metadata + waveform first; beatgrid required for sync/quantize; audio hash required for load.)
- How long should stale remote decks keep playing after disconnect? (Plan: continue, mark stale, offer manual mute; revisit after user testing.)
- Should the host be able to hand over the crossfader temporarily? (Later.)

## 17. Implementation decisions (2026-11-06)

These decisions refine the design above before implementation starts.

1. **The M0 spike uses the production building blocks.** `ClockSync`,
   `CommandScheduler`, `CollabProtocol`, and the control map are implemented as
   reusable classes. M0 adds a diagnostics panel and CSV logging around them
   instead of a throwaway socket spike.
2. **Clock sync is testable without two machines.** The clock source is
   injectable and the diagnostics harness can add an artificial offset and
   skew. A loopback test with one monotonic clock cannot validate offset or
   skew estimation by itself.
3. **Control identifiers are session-local.** The host assigns `1..N` to the
   negotiated control set and sends the complete `controlId -> ConfigKey` map
   at join. No hashing is used. A peer rejects unknown or missing controls.
4. **A collaboration session requires four decks.** The session sets
   `[App], num_decks` to 4 or refuses to start. Deck `orientation` values are
   saved on join and restored on leave because they are persistent controls.
5. **The remote-deck lock is best-effort.** The UI disables remote-deck
   widgets, and a per-control guard reverts changes that do not originate from
   the replica layer. A hardware controller can still produce a brief fight;
   the UI must show the locked state so the behaviour is visible.
6. **Reliable UDP messages may be fragmented.** `STATE_SNAPSHOT`,
   `CONTROL_KEYFRAME`, and other reliable messages can exceed the 1200-byte
   datagram limit. They use chunk headers with a reassembly timeout, or are
   sent over the TCP bulk channel when large.
7. **`SEEK` uses track frames.** `playposition` is a normalized control and is
   never replicated as a continuous control. The command layer converts
   between track frames and the local normalized control value. A late
   `PLAY` command seeks to the position the track would have at the current
   session time before starting.
8. **Every implementation commit builds and passes tests.** Milestone
   boundaries are tagged (`collab-m0` ... `collab-m4`) for bisection.

## 18. Decision log

| Date | Decision | Status |
|---|---|---|
| 2026-11-05 | Replicated deck-shadow, owner-authoritative per deck | Confirmed |
| 2026-11-05 | UDP control, separate TCP bulk | Confirmed |
| 2026-11-05 | Tailscale/WireGuard first | Confirmed |
| 2026-11-05 | Host-owned crossfader | Confirmed |
| 2026-11-05 | Scheduled 40–100 ms lead + immediate mode | Confirmed |
| 2026-11-05 | Opt-in Collab crate/playlist sharing | Confirmed |
| 2026-11-05 | No effects replication in MVP | Proposed |
| 2026-11-05 | No cross-peer beat sync in MVP | Proposed |
| 2026-11-05 | Analysis transfer with local-analysis fallback | Proposed |

> **AI-generated plan.** This document was written autonomously by an AI agent. Review and validate before implementation.
