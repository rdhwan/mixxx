# Goal — Online B2B Collaboration for Mixxx

> **AI-generated plan.** This document was written autonomously by an AI agent in collaboration with the user. Review, validate, and adapt it before implementation.

## 1. Problem

Two DJs want to perform B2B as if they were sharing one four-deck mixer, but from two separate machines. Each DJ controls two decks. Each DJ must hear their own local audio with no added latency. The friend's decks must also be audible locally, without streaming the playback audio over the network.

The missing pieces are:

- Low-latency replication of deck and mixer controls.
- A shared clock so "play/cue at time T" lands together on both machines.
- Correction for independent audio-device clocks, which drift over a long set.
- Automatic transfer of tracks that the other peer does not have.
- A safe way to gate playback until the other peer has a verified local copy.

## 2. Goal

Build an opt-in Mixxx feature that lets two peers join a shared four-deck session over a network where:

- Each peer's own two decks are local and authoritative.
- The other peer's two decks are local replicas driven by replicated state.
- All four decks play from local files on both machines.
- Audio is never streamed as the playback path.
- Control state, transport timing, and track files are synchronized.
- A track that the peer lacks is transferred ahead of time, verified, and then loaded; until then the deck is explicitly gated.
- The session remains coherent for at least a two-hour set.

## 3. Product principles

1. **Audio stays local.** The network carries control and files, never the live playback stream.
2. **The owner is authoritative.** Only the peer who owns a deck may change that deck's audio-affecting state. The follower mirrors and corrects; it does not fight the owner.
3. **Predictable timing beats raw speed.** A small, bounded scheduling lead is better than random beat placement.
4. **Fail visible, not silent.** If a peer is missing a track, unsynchronized, or disconnected, the UI says so and playback is gated or marked stale. No silent missing deck in the mix.
5. **Local monitoring stays local.** Headphone cue/PFL, headphone gain, master/booth gain, and output device are per-machine.
6. **Sharing is opt-in.** Only a designated Collab crate/playlist is prefetched by default. The whole library is never enumerated to the peer.
7. **Real-time safety.** No allocations, locks, or blocking calls in the audio callback.
8. **Modular and removable.** The feature is opt-in and isolated under `src/collab/`; disabling it leaves normal Mixxx behavior unchanged.

## 4. Non-goals for the MVP

- Host-authoritative audio with a return audio stream.
- Streaming playback audio (WebRTC/Opus, broadcast return, etc.).
- More than two peers.
- Public-internet operation without a VPN.
- Effects replication (per-deck or master).
- Cross-peer beat sync, auto-mixing, or AutoDJ coordination.
- Dynamic deck ownership. Fixed ownership is used for the MVP; takeover is a later milestone.
- Mobile, web, or broadcast integration.
- Automatic MIDI controller remapping beyond the documented deck mapping.
- A public track-sharing service or library indexing service.
- Sample-accurate engine-side scheduling in the first iteration. It is a fallback if M0 measurements require it.

## 5. Success criteria

These are the MVP acceptance targets. Exact tolerances are refined by M0 measurements.

### Timing

- Clock offset error between peers: p95 < 2 ms on a wired LAN, < 10 ms over Tailscale/consumer internet.
- Scheduled play/cue landing error: p95 within one audio buffer on each machine (5.8 ms at 256 frames / 44.1 kHz; 11.6 ms at 512 frames).
- Replica playhead error after correction: < 5 ms, with no audible correction artifact during normal play.
- Continuous control latency: p95 < 50 ms on LAN, < 100 ms over Tailscale.
- Two-hour session: no unbounded drift, no stuck deck, no control loop, no audio underrun attributable to the collaboration feature.

### Control

- Both peers can control their own two decks simultaneously.
- Transport, rate/tempo, keylock, loop, hotcue trigger, channel fader, pregain, EQ/filter, mute, and crossfader orientation replicate correctly.
- Host-owned crossfader is consistent on both machines; the guest cannot fight it.
- Remote decks are visibly locked and local input cannot silently change them.
- Local PFL/headphone and master/booth levels do not replicate.
- Disconnect/reconnect restores a coherent state within 2 seconds of network recovery.

### Track sharing

- If the peer already has the exact content hash, zero audio bytes are transferred.
- If the peer lacks it, the file transfers in resumable chunks and is fully SHA-256 verified before load.
- A dropped connection resumes without restarting the file.
- A corrupted or mismatched file is rejected and never loaded.
- Waveform/metadata can appear before the audio transfer completes.
- The owner's play is gated until the follower reports the track loaded.
- Bulk transfer does not increase control latency beyond the targets above and does not cause audio underruns.

### Performance

- Control traffic: < 50 kbit/s per peer at 60 Hz with deadband, excluding bulk transfer.
- Collaboration CPU overhead: < 5% of one core for control, clock, and scheduling on a typical desktop; hashing and transfer run on worker threads.
- No new mandatory dependency for the MVP. Qt Network, Qt Sql, and `QCryptographicHash` are already available.
- No changes to the audio callback in M1. If an engine-side scheduler is required, it is added behind a measured gate in M3.

## 6. User stories

1. **Start a session.** As DJ A, I open a Collab dialog, start a session, and share a session code with DJ B. I see B connect, with RTT and clock status.
2. **Join a session.** As DJ B, I enter the code, connect, and see which two decks are mine and which two are A's. My controller still controls my local decks 1/2.
3. **Load a track the peer has.** I load a track. The peer's corresponding deck loads the same hash immediately. Both decks show the same metadata and beatgrid.
4. **Load a track the peer lacks.** I load a track the peer does not have. The peer sees an incoming track with waveform/metadata and a progress indicator. My play button is gated until the peer reports ready.
5. **Play together.** I press play on a scheduled transition. Both machines start at the target time within one audio buffer. My own action is delayed by the configured lead (40–100 ms) only in scheduled mode.
6. **Mix.** I move my channel fader, EQ, and filter. The peer hears the same changes. The host moves the crossfader; both machines follow.
7. **Prefetch.** I select tracks in the Collab crate. They are hashed and offered to the peer; missing ones transfer at a capped rate in the background.
8. **Lose the network.** The network drops. Local audio continues; remote decks are marked stale. When the network returns, state re-anchors and transfers resume.
9. **Inspect.** I open a debug overlay and see RTT, jitter, clock offset, skew, anchor error, packet loss, late commands, and transfer rate.

## 7. Constraints

- Mixxx's audio callback is real-time: no allocations, locks, or blocking calls. The collaboration feature must not run there.
- `ControlObject`/`ControlDoublePrivate` values are atomic and can be read/written from multiple threads, but control-change handlers may run in the caller's thread. Remote application follows the same pattern as a controller. See `architecture.md`.
- Track loading, library access, and database writes happen on the main thread. `TrackCollectionManager` asserts thread affinity.
- The network thread must never block on the UI or on disk.
- Qt 6.2+ with Qt Network and Qt Sql is the baseline.
- No raw UDP/OSC/Ableton Link infrastructure exists in the tree. The protocol is new.
- The existing `src/network/` module is HTTP-task oriented; the collaboration module is separate under `src/collab/`.
- The project's AI policy requires human review and human submission of contributions. This plan is a design aid, not an autonomous contribution.

## 8. Key risks

| Risk | Impact | Mitigation |
|---|---|---|
| Audio-buffer quantization limits event alignment | Audible flam on kicks | M0 measurement; schedule to buffer boundaries; optional engine-side scheduled queue |
| Audio-device clock drift | Replica drifts against local decks | Periodic playhead anchors + rate nudge / short seek |
| NAT and consumer uplink | Poor first-run experience | Tailscale first; rendezvous/relay later; pacing and prefetch |
| Missing track at play time | Silent hole in the mix | Two-phase load commit + gating + prefetch |
| Analysis blob incompatibility | Wrong waveform/beatgrid | Version strings; fallback to local analysis |
| Effects not replicated | Different mix on each machine | Disable effects on remote decks in MVP; add later |
| Crossfader ownership UX | Guest cannot crossfade their own pair | Host-owned MVP; deck-pair crossfader later |
| Security of direct internet | Spoofing, replay, malware | VPN first + session token; authenticated encryption before public internet |
| Legal/privacy of file sharing | Copyright/privacy exposure | Trusted-peer, opt-in, no public indexing; user-initiated transfers |
| Upstream scope | Large feature may be hard to upstream | Keep modular, opt-in, well-tested; upstreaming is a separate decision |

## 9. Definition of done (MVP)

- M1 (control) and M2 (track sharing) acceptance criteria pass.
- M3 hardening passes a two-hour soak with packet loss and jitter.
- A human DJ test with two real machines and real controllers succeeds.
- The feature is off by default, documented, and can be disabled without affecting normal Mixxx.
- Known limitations are documented: no effects, host-owned crossfader, no public internet without VPN, fixed deck ownership.

> **AI-generated plan.** This document was written autonomously by an AI agent. Review and validate before implementation.
