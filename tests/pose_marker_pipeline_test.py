#!/usr/bin/env python
"""Verify marker insertion, profile generation, and full arm-branch targeting."""

from __future__ import annotations

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from branch_target_pipeline_test import synthetic_bundle
from build_ib_profile import file64_to_t48
from extract_runtime_profile import fnv1a
from pose_marker_pipeline import bundle_entries, read_bone_info


PATCH = "0123456789abcdef.patch_0"


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
             "pose-tree", "--root", str(source), "--out-root", str(output),
             "--left-translate", "0.04", "0.01", "0",
             "--right-translate", "-0.04", "0.01", "0"],
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
        if len(markers) != 2 or any(row[2:] != ["23", "8", "14", "8"] for row in markers):
            raise AssertionError(f"unexpected marker registry: {markers}")
        targets = [line.split() for line in
                   (profiles / "shoulder_targets.txt").read_text(encoding="utf-8").splitlines()
                   if line and not line.startswith("#")]
        if len(targets) != 12 or any(len(row) != 8 or
                                     row[0] != "POSE_CONTROL_FROM_SOURCE" for row in targets):
            raise AssertionError("pose targets do not map control slots to source slots")
        if any(int(row[3]) not in range(8, 14) or int(row[4]) not in range(1, 8)
               for row in targets):
            raise AssertionError("control/source slot ranges are wrong")
        if {tuple(row[5:]) for row in targets} != {
                ("+0.04", "+0.01", "+0"), ("-0.04", "+0.01", "+0")}:
            raise AssertionError("arm descendants did not receive one uniform request per side")

        bundle = (output / PATCH).read_bytes()
        entry = bundle_entries(bundle)[0]
        unit = bundle[entry["data_offset"]:entry["data_offset"] + entry["data_size"]]
        for lod in read_bone_info(unit)["lods"]:
            table_key = f"{fnv1a(file64_to_t48(lod['inverse_binds'])):016x}"
            control_for = {int(row[4]): int(row[3]) for row in targets
                           if row[1] == f"{entry['file_id']:016x}" and row[2] == table_key}
            expected_remap = [control_for.get(slot, slot) for slot in range(8)]
            if lod["remaps"] != [expected_remap]:
                raise AssertionError(f"LOD {lod['lod']} did not redirect arm remaps")

    print("pose marker copy/profile/full-branch pipeline passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
