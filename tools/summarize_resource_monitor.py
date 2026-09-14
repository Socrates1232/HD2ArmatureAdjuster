#!/usr/bin/env python3
"""Summarize one HD2 Armature Adjuster resource-monitor session."""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path


def number(row: dict[str, str], key: str) -> float:
    return float(row.get(key, 0) or 0)


def latest_monitor() -> Path:
    root = Path(os.environ["LOCALAPPDATA"]) / "HD2ArmatureAdjuster"
    candidates = list(root.glob("resource-monitor-*.csv"))
    if not candidates:
        raise FileNotFoundError(f"no resource monitor logs in {root}")
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def summarize(path: Path) -> dict[str, object]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"resource monitor log has no samples: {path}")

    first, last = rows[0], rows[-1]
    duration = max(0.0, (number(last, "unix_ms") - number(first, "unix_ms")) / 1000.0)

    def average(key: str) -> float:
        return sum(number(row, key) for row in rows) / len(rows)

    def peak(key: str) -> float:
        return max(number(row, key) for row in rows)

    return {
        "path": str(path.resolve()),
        "samples": len(rows),
        "duration_seconds": round(duration, 3),
        "process": {
            "cpu_percent_average": round(average("process_cpu_percent"), 3),
            "cpu_percent_peak": round(peak("process_cpu_percent"), 3),
            "working_set_mib_start": number(first, "working_set_mib"),
            "working_set_mib_end": number(last, "working_set_mib"),
            "working_set_mib_peak": peak("working_set_mib"),
            "private_mib_start": number(first, "private_mib"),
            "private_mib_end": number(last, "private_mib"),
            "private_mib_peak": peak("private_mib"),
        },
        "scanner": {
            "requests": int(number(last, "scan_requests")),
            "requests_coalesced": int(number(last, "scan_coalesced")),
            "runs": int(number(last, "scan_runs")),
            "full_runs": int(number(last, "full_scans")),
            "priority_runs": int(number(last, "priority_scans")),
            "total_mib_read": number(last, "scan_total_mib"),
            "wall_ms_peak_run": peak("scan_last_wall_ms"),
            "cpu_ms_peak_run": peak("scan_last_cpu_ms"),
            "cpu_core_percent_peak_sample": peak("scan_cpu_core_percent"),
            "cpu_core_percent_session": number(last, "scan_session_cpu_core_percent"),
        },
        "maintenance": {
            "calls": int(number(last, "maintenance_calls")),
            "table_checks": int(number(last, "maintenance_table_checks")),
            "total_mib_read": number(last, "maintenance_read_mib"),
            "wall_us_peak_call": peak("maintenance_max_us"),
            "wall_percent_peak_sample": peak("maintenance_wall_percent"),
        },
        "rebind": {
            "calls": int(number(last, "rebind_calls")),
            "candidates": int(number(last, "rebind_candidates")),
            "instances_added": int(number(last, "rebind_instances_added")),
            "wall_us_peak_call": peak("rebind_max_us"),
            "wall_percent_peak_sample": peak("rebind_wall_percent"),
        },
        "custom_worker": {
            "cycles": int(number(last, "custom_worker_cycles")),
            "busy_samples": sum(
                1 for row in rows if number(row, "custom_worker_busy") != 0
            ),
            "wall_us_last": number(last, "custom_worker_last_us"),
            "wall_us_peak_cycle": peak("custom_worker_max_us"),
            "wall_ms_total": number(last, "custom_worker_total_ms"),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", nargs="?", type=Path,
                        help="session CSV; defaults to the newest local session")
    args = parser.parse_args()
    path = args.path or latest_monitor()
    print(json.dumps(summarize(path), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
