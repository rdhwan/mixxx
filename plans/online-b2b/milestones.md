# Milestones — Online B2B Collaboration

> **AI-generated plan.** This document was written autonomously by an AI agent in collaboration with the user. Review, validate, and adapt it before implementation.

The milestones are ordered by risk. M0 is a measurement spike and a go/no-go gate. Do not build the full feature before the timing and drift assumptions are validated on real hardware.

## M0 — Spike and measurements

### Goal

Prove that the replicated deck-shadow model is viable with Mixxx's engine and two real machines. Measure the things that cannot be settled by reading code: scheduling jitter, audio-buffer quantization, clock-sync accuracy, audio-device drift, hashing speed, and the analysis-transfer path.

### Scope

- Two Mixxx instances, ideally on two machines with real audio interfaces.
- Reusable core building blocks (`ClockSync`, `CommandScheduler`,
  `CollabProtocol`, control map) plus a hidden diagnostics panel and CSV
  logging. M1 keeps these blocks; the harness is not throwaway code.
- An injectable clock source with artificial offset/skew so loopback tests can
  validate the estimator without two machines.
- A loopback audio measurement path (record both outputs or use a shared input) to measure actual event alignment.
- No track transfer in the main spike; a small file-copy/hash spike is enough.
- Run two instances on one machine with `--settings-path` and distinct ports
  for protocol tests; use two machines for the timing and drift gate.

### Deliverables

1. **Control-proxy proof**
   - Read and write `[ChannelN]` transport, rate, volume, EQ/filter, and `[Master], crossfader` from a non-UI thread.
   - Demonstrate echo suppression using `ControlProxy::set`.
   - Demonstrate playing and seeking a deck from a non-UI thread.
2. **Scheduling measurement**
   - Send "play at target time" to two instances.
   - Measure actual audio onset alignment at 128/256/512-frame buffers.
   - Report p50/p95/p99 and worst case.
3. **Clock-sync measurement**
   - Implement a minimal 4-timestamp ping exchange.
   - Measure offset/RTT/jitter over loopback, wired LAN, Wi-Fi, and Tailscale.
   - Report p50/p95 and stability over 10 minutes.
4. **Drift measurement**
   - Run two audio devices (or two machines) for 30–60 minutes with the same track.
   - Measure playhead drift without correction and with 1–2 s anchors.
5. **Hash performance**
   - SHA-256 throughput for MP3, FLAC, and WAV on the target machines.
   - Time to hash a typical 8 MB MP3 and a 100 MB WAV.
6. **Analysis spike**
   - Save/load `Waveform::toByteArray`, `Beats::toByteArray`, and cues for an imported track.
   - Confirm `AnalysisDao::saveAnalysis` / `saveTrackAnalyses` behavior.
7. **Decision memo**
   - Go/no-go for buffer-boundary scheduling.
   - If no-go, specify the engine-side scheduled-command queue design before M1.

### Acceptance criteria

- Control-proxy proof works from a non-UI thread without crashes or audio glitches.
- Scheduling p95 within one audio buffer on LAN. If not, the engine-side scheduler is added to M1 scope.
- Clock offset p95 < 2 ms on wired LAN; < 10 ms over Tailscale.
- Drift measurement shows the expected ppm difference and confirms anchors can hold the replica within 5 ms.
- Hashing a typical track is fast enough for prefetch (target: < 100 ms for an 8 MB MP3; large WAVs may take longer and are prefetched).
- The analysis spike either works or produces a concrete fallback plan.

### Tests

- Manual two-machine test with a loopback audio capture or an oscilloscope-style recording.
- `tc netem` on Linux for 20 ms / 50 ms RTT and 1% loss.
- Repeated runs at different buffer sizes.

### Gate

**If scheduled play alignment p95 is worse than ~15 ms on a wired LAN, stop and design the engine-side scheduled-command queue before M1.** Do not compensate with a larger lead; that only delays the owner's action and does not fix the quantization.

### Out of scope

- Track transfer UI.
- Library integration.
- Authentication/encryption.
- Effects.

## M1 — LAN control replication

### Goal

Two peers on a LAN or Tailscale can run a shared four-deck session with no track sharing. Both peers must already have the tracks. Transport, rate, loop, channel strip, and crossfader replicate with bounded latency and stable drift correction.

### Scope

- Production-ish `CollabSession`, `CollabProtocol`, `ClockSync`, `CommandScheduler`, `ControlReplicator`, `DeckMap`, `DeckStateReplicator`.
- Session dialog: start/join, session code, peer status.
- Deck ownership: host 1/2, guest 3/4; literal session deck N = local `ChannelN` on both peers, so the guest's own decks are `Channel3/4`.
- Remote deck lock: local input ignored/reverted with a visible indicator.
- Replicated controls: play/pause, seek, rate/tempo, keylock, loop, hotcue trigger, channel volume, pregain, EQ/filter, mute, crossfader orientation.
- Host-owned crossfader; guest crossfader disabled/overridden.
- Local-only: PFL/headphone, master/booth gain, output device.
- Clock sync with burst + 1 Hz; session time; scheduled and immediate modes.
- Playhead anchors every 1 s (plus transport changes); drift correction with a hard seek when the error reaches 5 ms. Rate nudging is deferred.
- Keyframes every 5 s; full snapshot on join/reconnect.
- Debug overlay with RTT, jitter, offset, skew, anchor error, late commands, packet loss.
- Reconnect handling: local audio continues, remote decks marked stale, re-anchor on return.

### Deliverables

1. `src/collab/` module with the components above.
2. Protocol implementation with unit tests.
3. Deck mapping and control-scope enforcement.
4. Minimal session UI and debug overlay.
5. Two-instance integration test over loopback.
6. Manual two-machine test report.

### Acceptance criteria

- Two peers join, see correct ownership, and control their own two decks.
- Remote decks are locked; local input does not silently change them.
- Play/pause/seek/rate/loop/hotcue replicate.
- Channel fader, pregain, EQ/filter, mute replicate.
- Host crossfader is consistent on both machines; guest cannot fight it.
- Scheduled play lands within one audio buffer p95 on LAN.
- Replica drift stays < 5 ms over a 30-minute session.
- Clock offset p95 < 2 ms on wired LAN; < 10 ms over Tailscale.
- Control latency p95 < 50 ms on LAN; < 100 ms over Tailscale.
- No audio underruns attributable to collaboration.
- Disconnect/reconnect restores state within 2 seconds of network recovery.
- Debug overlay reports the expected metrics.

### Tests

- Unit: protocol, clock filter, scheduler, control map, echo suppression, keyframe epochs.
- Integration: two `CollabSession` instances on loopback with a mocked control bus.
- `tc netem`: 20 ms RTT, 1% loss, reordering; 50 ms RTT + 20 ms jitter, 2% loss.
- Manual: 30 minutes, all four decks, real controllers, real audio interfaces.
- Fault injection: kill peer, drop network for 2 s, duplicate/reorder packets, clock jump.

### Out of scope

- Track transfer.
- Waveform/analysis transfer.
- Effects.
- Dynamic ownership/takeover.
- Public-internet rendezvous/relay.
- Deck-pair crossfader.

### Gate

M1 is complete only when the two-machine manual test passes. A loopback-only success is not enough.

## M2 — Track sharing and load gating

### Goal

A peer can load a track the other peer does not have. The missing file transfers in the background, resumes after interruption, verifies, and becomes loadable only when complete. Prefetch makes the common case instant.

### Scope

- Content hash index (SHA-256) with path/size/mtime invalidation.
- Content-addressed cache in the Collab folder.
- Offer/need/have handshake.
- Prefetch on selection, next track, and Collab crate/playlist push.
- Opt-in sharing policy; no whole-library enumeration.
- TCP bulk channel: chunks, bitmap/durable offset, resume, pacing, backpressure.
- Full hash verification, temp file, atomic install.
- WAV/AIFF → FLAC in transit; MP3/FLAC/OGG raw; fallback to raw.
- Two-phase load: `LOAD_REQUEST` → `NEED/READY` → transfer → `LOAD_READY`.
- Play gating until the follower is ready.
- Analysis transfer: waveform, waveform summary, beats, cues; version fallback.
- Waveform-first incoming row or equivalent UI.
- Library import via `TrackCollectionManager::getOrAddTrack`.
- Transfer progress panel with cancel/retry.
- Disk-space preflight and sanitized filenames.

### Deliverables

1. Hash index and cache with tests.
2. TCP transfer service with resume/verify/pacing.
3. Prefetch pipeline and sharing policy UI.
4. Load-gating handshake.
5. Analysis transfer and fallback.
6. Incoming/waveform UI.
7. Library import and Collab crate.
8. Two-machine transfer test report.

### Acceptance criteria

- Peer has the hash: zero audio bytes transferred; deck loads immediately.
- Peer lacks the hash: transfer starts, shows progress, and loads only after full verification.
- Kill the connection mid-transfer: resume does not restart from zero.
- Corrupt a chunk: full hash fails, temp file is deleted, error is shown, retry works.
- Disk full: transfer is rejected before writing; no partial file is installed.
- Hash mismatch never becomes loadable.
- Waveform/metadata are visible before the audio transfer completes.
- Owner play is gated until `LOAD_READY`.
- Bulk transfer does not increase control latency beyond M1 targets and does not cause audio underruns.
- Prefetch on selection and next track works; the Collab crate push works.
- Analysis version mismatch falls back to local analysis without breaking load.
- Cancel/retry is reliable.

### Tests

- Unit: hash invalidation, cache lookup, bitmap/resume, chunk boundaries, filename sanitization, disk-space check.
- Integration: 1 MB, 10 MB, 100 MB, and 1 GB files; slow link; disconnect/reconnect; kill receiver; kill sender.
- `tc netem`: loss/jitter during transfer while control traffic is active.
- Hash corruption and truncated file.
- Analysis blob round-trip for waveform, beats, and cues.
- Manual: load a missing track during active mixing; confirm gating and no control latency spike.

### Out of scope

- Public-internet relay.
- Effects.
- Multi-peer sharing.
- Background scanning of the entire library by default (optional later).

### Gate

M2 is complete when a missing track can be loaded end-to-end on two machines, with resume and verification, without disrupting the control channel.

## M3 — Hardening and long-session behavior

### Goal

Make the system survive real network conditions and long sets. Eliminate stuck states, unbounded memory, correction artifacts, and recovery failures.

### Scope

- Packet loss/jitter/reordering/late-command hardening.
- Anchor correction tuning; test rate nudge vs hard seek for audible artifacts.
- Clock resync and skew handling.
- State reconciliation after reconnect and ownership changes.
- Duplicate/out-of-order command handling.
- Session epoch handling and stale-packet rejection.
- Memory bounds: queues, retransmit buffers, hold-back queues, transfer windows.
- Logging, metrics, and a support bundle.
- Crash/restart behavior.
- Optional: engine-side scheduled-command queue if M0/M1 showed it is needed.
- Optional: takeover request/grant.
- Optional: crossfader handover.

### Deliverables

1. Hardened protocol and scheduler.
2. Soak-test harness and network fault profiles.
3. Metrics/logging and debug overlay improvements.
4. Long-session report.
5. Recovery playbook for common failures.

### Acceptance criteria

- Two-hour session with 2% loss and 50 ms jitter: no desync > 10 ms, no stuck deck, no control loop, no unbounded memory.
- Disconnect for 10 s, reconnect: state restores, transfer resumes, deck re-anchors.
- Clock jump of 100 ms: future events reschedule; already-scheduled events do not fire wildly.
- Duplicate commands apply once.
- Late commands follow the documented policy.
- Anchor correction is inaudible in normal play; correction count and error are logged.
- No audio underruns in a two-hour soak.
- Crash/restart of one peer does not corrupt the other peer's state or cache.

### Tests

- Soak: 2 hours, 4 decks, 60 Hz fader movement, periodic track loads, transfers.
- `tc netem` matrix: 0/1/2/5% loss, 0/20/50 ms jitter, reorder, duplicate.
- Fault injection: clock jump, peer kill, network partition, disk full, hash mismatch, protocol fuzz.
- Memory profiling: queues, retransmit buffers, transfer windows.
- Audio analysis: listen for clicks/glitches during corrections.

### Out of scope

- Public-internet relay.
- Effects replication.
- More than two peers.

## M4 — Internet/productization

### Goal

Make the feature usable by two people who are not on the same LAN, with a clear setup path and a security posture appropriate for a public release.

### Scope

- Tailscale onboarding: setup help, connection status, troubleshooting.
- Session codes/invites and a simple join flow.
- Rendezvous service and/or relay for peers who cannot use Tailscale (decision after user testing).
- Authenticated encryption for direct connections (Noise/QUIC/DTLS; TLS/Noise for TCP).
- Security review: replay, spoofing, path traversal, malicious files, denial of service.
- Preferences, docs, and error messages.
- Library integration polish: Collab crate, cleanup policy, cache size.
- Packaging and platform testing (Linux/macOS/Windows).
- Optional telemetry/log collection with explicit consent.

### Deliverables

1. Tailscale-first onboarding.
2. Public-internet transport design and implementation (or a documented Tailscale-only decision).
3. Security review and fixes.
4. User documentation and troubleshooting guide.
5. Platform test matrix.

### Acceptance criteria

- Two peers on different NATs connect via Tailscale in under 5 minutes with the docs.
- If relay is implemented: two peers without Tailscale connect through the relay.
- Direct connections are authenticated and encrypted; no plaintext control or file data.
- Malicious path/filename/dataset tests are rejected.
- Session survives a network change (Wi-Fi to wired, IP change) within a bounded time.
- Docs cover setup, deck ownership, sharing policy, gating, and known limitations.
- No regressions in normal Mixxx operation when collaboration is disabled.

### Out of scope

- More than two peers.
- Effects replication.
- Mobile/web clients.

## M5 — Optional extensions

These are explicitly post-MVP and should be prioritized by user feedback.

- FLAC transit as a default for WAV/AIFF after CPU/quality validation.
- Per-deck effects replication.
- Master effects replication.
- Cross-peer beat sync with an explicit sync-authority model.
- Deck-pair crossfader (host pair vs guest pair).
- Dynamic deck ownership and takeover.
- More than two peers (requires routing, ownership, and clock topology).
- WebRTC/QUIC transport.
- Kernel timestamping and sample-accurate engine scheduling.
- Mobile/tablet controller support.
- Session recording/broadcast integration.

## Cross-cutting test plan

### Unit

- Protocol serialization/deserialization and fuzzing.
- Clock filter and skew regression.
- Command scheduler and late policy.
- Control map, deck map, echo suppression.
- Hash index/cache.
- Transfer bitmap/resume/verification.
- State version/epoch handling.

### Integration

- Two sessions over loopback.
- Two Mixxx instances with separate settings paths.
- Mocked engine/control bus for deterministic tests.
- `tc netem` profiles.
- Fault injection.

### Manual

- Two real machines, two controllers, real audio interfaces.
- 30-minute and 2-hour sessions.
- Same-room and remote.
- Track load with/without the file; transfer during active mixing.
- Disconnect/reconnect; sleep/wake; network change.

### Metrics

- RTT, jitter, clock offset, skew.
- Anchor error and correction count.
- Late/duplicate/retransmitted commands.
- Packet loss.
- Transfer rate, resume count, verification failures.
- Audio latency overload count/usage.
- CPU and memory.

## Release criteria for the MVP

- M1 + M2 + M3 acceptance criteria pass.
- A human DJ test with two real machines and real controllers succeeds.
- The feature is off by default and does not affect normal Mixxx when disabled.
- Known limitations are documented.
- No open security issue in the threat model.
- The code is modular, tested, and reviewable.

## Suggested order of implementation within M1

1. `DeckMap` + control scope + `ControlReplicator` on loopback.
2. `CollabProtocol` + UDP session + reliability.
3. `ClockSync` + `CommandScheduler` + scheduled play.
4. `DeckStateReplicator` + anchors + drift correction.
5. UI + debug overlay.
6. Reconnect + fault handling.

Do not start with track transfer. Control timing is the harder risk.

## Suggested order within M2

1. Hash index + cache.
2. TCP transfer + resume + verify.
3. Offer/need/have + prefetch.
4. Load gating.
5. Analysis transfer + waveform-first.
6. Library import + Collab crate.
7. Pacing/backpressure + priorities.

## Definition of done

The MVP is done when two DJs can run a two-hour B2B set from two machines, each controlling two decks, with local audio, synchronized transport and mixer state, automatic transfer of missing tracks, visible gating, and no audio dropouts or silent desync.

> **AI-generated plan.** This document was written autonomously by an AI agent. Review and validate before implementation.
