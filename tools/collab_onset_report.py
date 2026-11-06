#!/usr/bin/env python3
"""Measure the audio onset difference between the two B2B peers.

The M0 gate is an audio measurement, not a clock measurement: both machines
must start a deck at the same session time, and the two outputs must reach the
same recording device within one audio buffer.

Record the two outputs into the two channels of one stereo WAV file (loopback
interface, small mixer, or a two-input sound card). Each scheduled play is one
event: silence before the deck starts, the track afterwards.

Usage:
    tools/collab_onset_report.py recording.wav
    tools/collab_onset_report.py recording.wav --frame-size 512 --sample-rate 44100
    tools/collab_onset_report.py recording.wav --channel-a-left --window-ms 2

The script prints one line per event and the percentile summary the milestone
gate uses. It never writes to the recording.
"""

from __future__ import annotations

import argparse
import array
import statistics
import sys
import wave
from pathlib import Path


def error(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def read_wav(path: Path) -> tuple[list[float], list[float], int]:
    """Return left and right samples normalised to [-1, 1], plus sample rate."""
    try:
        with wave.open(str(path), "rb") as wav:
            channels = wav.getnchannels()
            if channels != 2:
                error(
                    f"{path} has {channels} channels; record both peers into "
                    "one stereo file"
                )
            sample_rate = wav.getframerate()
            sample_width = wav.getsampwidth()
            frames = wav.readframes(wav.getnframes())
    except wave.Error as exc:
        error(f"cannot read {path} as PCM WAV: {exc}")

    if sample_width == 2:
        data = array.array("h")
        data.frombytes(frames)
        scale = 32768.0
    elif sample_width == 4:
        data = array.array("i")
        data.frombytes(frames)
        scale = 2147483648.0
    elif sample_width == 3:
        # 24-bit PCM: widen to 32-bit integers.
        data = array.array("i")
        for index in range(0, len(frames), 3):
            value = int.from_bytes(frames[index:index + 3], "little", signed=True)
            data.append(value)
        scale = 8388608.0
    elif sample_width == 1:
        data = array.array("b")
        data.frombytes(frames)
        data = array.array("i", (value - 128 for value in data))
        scale = 128.0
    else:
        error(f"unsupported sample width: {sample_width * 8} bit")

    left = [data[index] / scale for index in range(0, len(data), 2)]
    right = [data[index + 1] / scale for index in range(0, len(data), 2)]
    return left, right, sample_rate


def envelope(samples: list[float], window: int) -> list[float]:
    """Peak envelope per window, so a single click cannot miss the threshold."""
    peaks = []
    for start in range(0, len(samples) - window + 1, window):
        peaks.append(max(abs(value) for value in samples[start:start + window]))
    return peaks


def detect_onsets(
    samples: list[float], window: int, min_gap_windows: int
) -> list[int]:
    """Return the sample index of every rising edge above the noise floor.

    The threshold is derived from the file itself: four times the median
    envelope (the silence between events) but at least 3 % of the loudest
    window. After a rising edge, further edges are suppressed for
    `min_gap_windows`, so one play is one event even when the track keeps
    sounding.
    """
    peaks = envelope(samples, window)
    if not peaks:
        return []

    ordered = sorted(peaks)
    # The floor is a low percentile, not the median: a recording may be mostly
    # music with only a few seconds of silence between events.
    noise_floor = ordered[max(0, len(ordered) // 20)]
    loudest = ordered[-1]
    threshold = max(noise_floor * 4.0, loudest * 0.03)
    if threshold <= 0.0:
        return []

    # Precise onset level: just above the noise floor, so the first audible
    # sample is found even when the track starts with a slow swell.
    level = max(noise_floor * 2.0, loudest * 0.002)

    onsets = []
    index = 0
    while index < len(peaks):
        if peaks[index] < threshold:
            index += 1
            continue
        # Only a transition from below to above the threshold is an onset.
        if index > 0 and peaks[index - 1] >= threshold:
            index += 1
            continue
        # Refine to the first sample above the precise level in the window
        # around the rising edge. The same rule is applied to both channels, so
        # its small systematic bias cancels in the difference.
        start = max(0, (index - 1) * window)
        stop = min(len(samples), (index + 1) * window)
        exact = start
        for sample_index in range(start, stop):
            if abs(samples[sample_index]) >= level:
                exact = sample_index
                break
        onsets.append(exact)
        index += min_gap_windows
    return onsets


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path, help="stereo WAV with both peers")
    parser.add_argument(
        "--sample-rate",
        type=int,
        default=0,
        help="override the sample rate printed with the result",
    )
    parser.add_argument(
        "--window-ms",
        type=float,
        default=2.0,
        help="envelope window in milliseconds (default: 2)",
    )
    parser.add_argument(
        "--min-gap-ms",
        type=float,
        default=300.0,
        help="minimum distance between two events (default: 300)",
    )
    parser.add_argument(
        "--frame-size",
        type=int,
        default=0,
        help="audio buffer size, used only to report the gate verdict",
    )
    args = parser.parse_args()

    if not args.recording.is_file():
        error(f"{args.recording} does not exist")

    left, right, sample_rate = read_wav(args.recording)
    if args.sample_rate > 0:
        sample_rate = args.sample_rate
    if sample_rate <= 0:
        error("sample rate must be positive")

    window = max(1, int(round(sample_rate * args.window_ms / 1000.0)))
    min_gap_windows = max(1, int(round(args.min_gap_ms / args.window_ms)))

    onsets_a = detect_onsets(left, window, min_gap_windows)
    onsets_b = detect_onsets(right, window, min_gap_windows)

    print(f"file: {args.recording}")
    print(f"sample rate: {sample_rate} Hz")
    print(f"peer A onsets: {len(onsets_a)}   peer B onsets: {len(onsets_b)}")

    events = min(len(onsets_a), len(onsets_b))
    if events == 0:
        print(
            "\nNo onset found. Record with silence before each play and make "
            "sure both peers are on separate channels.",
            file=sys.stderr,
        )
        return 1
    if len(onsets_a) != len(onsets_b):
        print(
            f"\nwarning: the channels produced different event counts "
            f"({len(onsets_a)} vs {len(onsets_b)}); comparing the first "
            f"{events}",
            file=sys.stderr,
        )

    deltas_ms = []
    print("\n event   peer A (s)   peer B (s)   B - A (ms)")
    for index in range(events):
        delta = (onsets_b[index] - onsets_a[index]) / sample_rate * 1000.0
        deltas_ms.append(delta)
        print(
            f"{index + 1:>6}   {onsets_a[index] / sample_rate:>10.6f}   "
            f"{onsets_b[index] / sample_rate:>10.6f}   {delta:>+10.3f}"
        )

    absolute = [abs(value) for value in deltas_ms]
    print(
        f"\nonset difference  n={len(absolute)}  "
        f"p50={percentile(absolute, 0.50):6.3f} ms  "
        f"p95={percentile(absolute, 0.95):6.3f} ms  "
        f"max={max(absolute):6.3f} ms  "
        f"mean={statistics.fmean(absolute):6.3f} ms"
    )
    print(f"signed mean: {statistics.fmean(deltas_ms):+.3f} ms (a constant offset is calibratable)")

    if args.frame_size > 0:
        buffer_ms = args.frame_size / sample_rate * 1000.0
        p95 = percentile(absolute, 0.95)
        verdict = "PASS" if p95 <= buffer_ms else "FAIL"
        print(
            f"gate at {args.frame_size} frames ({buffer_ms:.2f} ms): "
            f"{verdict} (p95 {p95:.3f} ms)"
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
