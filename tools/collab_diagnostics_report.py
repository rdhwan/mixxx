#!/usr/bin/env python3
"""Summarize a Mixxx collaboration diagnostics CSV file.

Usage:
    tools/collab_diagnostics_report.py collab-diagnostics.csv

The CSV is written by the M0 diagnostics probe (Settings > Online B2B) and
contains clock samples and, once scheduling tests are added, scheduled command
results. The script prints latency percentiles that the milestone gate uses.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize(name: str, values: list[float], unit: str) -> None:
    if not values:
        print(f"{name:<24} no samples")
        return
    print(
        f"{name:<24} n={len(values):<6} "
        f"p50={percentile(values, 0.50):8.3f} {unit}  "
        f"p95={percentile(values, 0.95):8.3f} {unit}  "
        f"p99={percentile(values, 0.99):8.3f} {unit}  "
        f"min={min(values):8.3f}  max={max(values):8.3f}  "
        f"mean={statistics.fmean(values):8.3f}"
    )


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def as_float(row: dict[str, str], key: str) -> float | None:
    value = row.get(key, "")
    if value in ("", None):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path, help="diagnostics CSV file")
    args = parser.parse_args()

    if not args.csv_file.is_file():
        print(f"error: {args.csv_file} does not exist", file=sys.stderr)
        return 1

    rows = read_rows(args.csv_file)
    if not rows:
        print("error: no rows", file=sys.stderr)
        return 1

    clock_rows = [row for row in rows if row.get("kind") == "clock"]
    command_rows = [row for row in rows if row.get("kind") == "command"]

    rtt_ms = [value / 1000.0 for row in clock_rows
              if (value := as_float(row, "rtt_us")) is not None]
    offset_ms = [value / 1000.0 for row in clock_rows
                 if (value := as_float(row, "offset_us")) is not None]
    jitter_ms = [value / 1000.0 for row in clock_rows
                 if (value := as_float(row, "jitter_us")) is not None]
    skew_ppm = [value for row in clock_rows
                if (value := as_float(row, "skew_ppm")) is not None]

    lateness_ms = [value / 1000.0 for row in command_rows
                   if (value := as_float(row, "lateness_us")) is not None]
    dropped = sum(1 for row in command_rows if row.get("dropped") == "1")

    print(f"file: {args.csv_file}")
    print(f"clock samples: {len(clock_rows)}   command samples: {len(command_rows)}")
    print()
    summarize("RTT", rtt_ms, "ms")
    summarize("offset", offset_ms, "ms")
    summarize("jitter", jitter_ms, "ms")
    summarize("skew", skew_ppm, "ppm")
    print()
    summarize("command lateness", lateness_ms, "ms")
    print(f"{'dropped commands':<24} {dropped}")

    print()
    print("M0 gate: scheduled play p95 must be within one audio buffer on a")
    print("wired LAN. See plans/online-b2b/milestones.md.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
