#!/usr/bin/env python
"""Verify marker insertion, profile generation, and full arm-branch targeting."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

from branch_target_pipeline_test import synthetic_bundle


PATCH = "0123456789abcdef.patch_0"
ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        source = pathlib.Path(temporary) / "source"
        output = pathlib.Path(temporary) / "output"
        source.mkdir()
        original = synthetic_bundle(False)
        (source / PATCH).write_bytes(original)
        (source / f"{PATCH}.gpu_resources").write_bytes(b"gpu unchanged")
        (source / f"{PATCH}.stream").write_bytes(b"stream unchanged")

        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "patch_profile_tool.py"),
             "pose-tree", "--root", str(source), "--out-root", str(output)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        if completed.returncode:
            raise AssertionError(completed.stdout)
        summary = json.loads(completed.stdout)
        expected = {"patches_marked": 1, "marker_variants": 1, "marker_tables": 2,
                    "profiles_active": 1, "arm_targets": 12}
        if any(summary.get(key) != value for key, value in expected.items()):
            raise AssertionError(summary)

        if digest((source / PATCH).read_bytes()) != digest(original):
            raise AssertionError("source patch was modified")
        if len((output / PATCH).read_bytes()) <= len(original):
            raise AssertionError("marked patch did not grow")
        for suffix, expected in (("gpu_resources", b"gpu unchanged"),
                                 ("stream", b"stream unchanged")):
            if (output / f"{PATCH}.{suffix}").read_bytes() != expected:
                raise AssertionError(f"{suffix} companion changed")

        profiles = output / "HD2ArmatureProfiles"
        markers = [line.split() for line in
                   (profiles / "palette_markers.txt").read_text(encoding="utf-8").splitlines()
                   if line and not line.startswith("#")]
        if len(markers) != 2 or any(row[2:] != ["17", "8", "8"] for row in markers):
            raise AssertionError(f"unexpected marker registry: {markers}")
        targets = [line.split() for line in
                   (profiles / "shoulder_targets.txt").read_text(encoding="utf-8").splitlines()
                   if line and not line.startswith("#")]
        if len(targets) != 12 or any(int(row[2]) >= 8 for row in targets):
            raise AssertionError("marker slots leaked into arm targets")
        if {tuple(row[3:]) for row in targets} != {
                ("+0.03", "+0", "+0"), ("-0.03", "+0", "+0")}:
            raise AssertionError("arm descendants did not receive one uniform request per side")

    print("pose marker copy/profile/full-branch pipeline passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
