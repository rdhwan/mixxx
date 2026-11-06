# M0 results — Online B2B Collaboration

> **AI-generated document.** This file was written autonomously by an AI agent
> from the diagnostics CSVs the contributor recorded. Review it before using it
> as gate evidence.

Raw data and the analysis tool are not part of this document: the CSVs live in
`diagnostics/`, and `tools/collab_diagnostics_report.py` prints the percentiles
below.

## Run 1 — clock probe only

| | Host | Guest |
|---|---|---|
| Machine | Linux laptop `192.168.101.12` | Windows PC `192.168.101.52` |
| Commit | `dc52a4c447` | `dc52a4c447` |
| Duration | 436.8 s | 436.8 s |
| Probe interval | 200 ms | 200 ms |
| Clock samples | 2185 | 2183 |
| Lost exchanges | ~0 % | ~0 % |

| Metric | Host | Guest |
|---|---|---|
| RTT p50 | 4.13 ms | 2.43 ms |
| RTT p95 | 17.35 ms | 11.92 ms |
| RTT p99 | 66.16 ms | 38.11 ms |
| RTT max | 436.46 ms | 561.47 ms |
| Jitter p50 | 0.25 ms | 0.10 ms |
| Skew estimate p50 | −18.0 ppm | +22.1 ppm |

### Connection and command transport

Both peers reached the connected state, so the IPv4-mapped IPv6 fix works.
One click on **Schedule play** produced one command that ran on both peers:

| Peer | Command id | Target (session µs) | Lateness |
|---|---|---|---|
| Host | 2 | 33 838 000 000 | 0.215 ms |
| Guest | 2 | 33 838 000 000 | 4.702 ms |

The two older command rows in the host CSV (`command_id` 1 and 2, targets
32 328 s and 32 520 s) are from the earlier run where the probe never
connected; they are not part of this run.

### Findings

1. **Relative clock drift is ≈ 20 ppm**, about 1.2 ms per minute, or 72 ms per
   hour. The offset moves linearly in both logs (mirrored), so it is genuine
   quartz drift, not estimator noise. Continuous re-estimation is therefore
   mandatory; a single offset per session would be useless after a few minutes.
2. **Command lateness is dominated by the scheduler poll interval, not the
   network.** The guest's 4.702 ms is one tick of the old 5 ms timer. The host's
   0.215 ms is a lucky phase. The scheduler timer is now 1 ms.
3. **RTT has a periodic tail.** Both logs show spikes roughly every 10.8 s,
   some of them 30–560 ms. A third to a half of the spikes coincide in absolute
   time on both peers (common cause, likely the shared link or power
   management); the rest are local to one machine. This is not behaviour of a
   quiet wired LAN and must be explained before the audio gate is judged.
4. **Clock samples are not audio samples.** The CSV records when the userspace
   scheduler ran, not when the audio device produced the first sample. The
   actual onset also includes the engine callback and each device's output
   latency, which are different on the two machines.

### Not measured yet

- Audio onset difference at 128, 256, and 512-frame buffers (the actual M0
  gate).
- Drift over 30–60 minutes without interruption.
- Hash throughput and the analysis-transfer spike.

### Verdict

The M0 gate has **not** passed. The clock and transport layers look viable, but
the gate item — scheduled play onset within one audio buffer on a wired LAN —
has no audio measurement yet.
