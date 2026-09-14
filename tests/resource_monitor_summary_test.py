#!/usr/bin/env python3

import csv
import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    fields = [
        "unix_ms", "process_cpu_percent", "working_set_mib", "private_mib",
        "scan_requests", "scan_coalesced", "scan_runs", "full_scans",
        "priority_scans", "scan_total_mib", "scan_last_wall_ms",
        "scan_last_cpu_ms", "scan_cpu_core_percent",
        "scan_session_cpu_core_percent", "maintenance_calls",
        "maintenance_table_checks", "maintenance_read_mib", "maintenance_max_us",
        "maintenance_wall_percent", "rebind_calls", "rebind_candidates",
        "rebind_instances_added", "rebind_max_us", "rebind_wall_percent",
        "palette_scan_calls", "palette_scan_total_mib", "palette_scan_max_ms",
        "palette_scan_wall_percent", "palette_marker_hits", "pose_driver_calls",
        "pose_driver_max_us", "pose_driver_wall_percent", "pose_updates",
        "pose_update_skips",
    ]
    rows = [
        dict.fromkeys(fields, "0") | {
            "unix_ms": "1000", "process_cpu_percent": "10",
            "working_set_mib": "8000", "private_mib": "10000",
            "scan_cpu_core_percent": "3", "maintenance_wall_percent": "0.1",
        },
        dict.fromkeys(fields, "0") | {
            "unix_ms": "2000", "process_cpu_percent": "20",
            "working_set_mib": "8100", "private_mib": "10100",
            "scan_requests": "3", "scan_coalesced": "1", "scan_runs": "2",
            "full_scans": "1", "priority_scans": "1", "scan_total_mib": "64",
            "scan_last_wall_ms": "12", "scan_last_cpu_ms": "8",
            "scan_cpu_core_percent": "7", "scan_session_cpu_core_percent": "2.5",
            "maintenance_calls": "60", "maintenance_table_checks": "120",
            "maintenance_read_mib": "4", "maintenance_max_us": "30",
            "maintenance_wall_percent": "0.2", "rebind_calls": "2",
            "rebind_candidates": "6", "rebind_instances_added": "2",
            "rebind_max_us": "40", "rebind_wall_percent": "0.05",
            "palette_scan_calls": "60", "palette_scan_total_mib": "240",
            "palette_scan_max_ms": "2.5", "palette_scan_wall_percent": "4.0",
            "palette_marker_hits": "12", "pose_driver_calls": "10",
            "pose_driver_max_us": "80", "pose_driver_wall_percent": "0.08",
            "pose_updates": "8", "pose_update_skips": "2",
        },
    ]
    with tempfile.TemporaryDirectory() as temporary:
        path = pathlib.Path(temporary) / "monitor.csv"
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "summarize_resource_monitor.py"), str(path)],
            check=True, capture_output=True, text=True,
        )
        result = json.loads(completed.stdout)

    assert result["duration_seconds"] == 1.0
    assert result["process"]["cpu_percent_average"] == 15.0
    assert result["process"]["working_set_mib_peak"] == 8100.0
    assert result["scanner"]["runs"] == 2
    assert result["scanner"]["cpu_core_percent_peak_sample"] == 7.0
    assert result["palette_scanner"]["marker_hits"] == 12
    assert result["pose_driver"]["updates"] == 8
    assert result["maintenance"]["wall_us_peak_call"] == 30.0
    assert result["rebind"]["instances_added"] == 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
