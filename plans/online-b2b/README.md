# Online B2B Collaboration — Plan

> **AI-generated plan.** This document was written autonomously by an AI agent in collaboration with the user. Review, validate, and adapt it before implementation. It is not a substitute for human design review or real DJ testing.

Status: **Accepted; implementation started 2026-11-06**
Date: **2026-11-05**
Repository: `/home/faridh/Projects/mixxx`
Branch at planning time: `online-multi`

## Confirmed decisions

| Decision | Choice |
|---|---|
| Playback architecture | Replicated deck-shadow, owner-authoritative per deck |
| Bulk transfer | Separate TCP side channel |
| Connectivity | Tailscale/WireGuard first; rendezvous/relay later |
| Crossfader | Host owns the shared crossfader |
| Timing | Scheduled 40–100 ms lead + immediate mode |
| Sharing policy | Opt-in "Collab" crate/playlist, not the whole library |

## Files

- [`goal.md`](goal.md) — problem, goal, non-goals, measurable success criteria.
- [`architecture.md`](architecture.md) — system design, threading, deck mapping, control scope, drift correction, track sharing, UI, risks.
- [`protocol.md`](protocol.md) — UDP control protocol, clock sync, reliability, anchors, TCP bulk transfer, message flows.
- [`milestones.md`](milestones.md) — M0–M5 scope, deliverables, acceptance criteria, test plan.

## One-paragraph summary

Two Mixxx instances join a shared four-deck session. Each peer owns two decks and is authoritative for them; the other two decks are replicas played from local files. Audio never crosses the network in the playback path. A UDP control plane replicates transport, mixer, and timing state; an NTP-style clock exchange stamps commands with target times; periodic playhead anchors correct replica drift caused by independent audio clocks. A separate TCP channel transfers missing tracks into a content-addressed cache, with resumable chunks, full SHA-256 verification, prefetching, and load gating. The first target is two peers on a LAN or Tailscale network; public-internet NAT traversal and relay are a later milestone.

## Next action

Run **M0** in [`milestones.md`](milestones.md): measure scheduling jitter, clock-sync accuracy, and drift on two real machines. The M0 result is a go/no-go gate for the rest of the plan.

> **AI-generated plan.** This document was written autonomously by an AI agent. Review and validate before implementation.
