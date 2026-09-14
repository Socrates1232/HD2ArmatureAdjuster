#!/usr/bin/env python
"""Verify clavicle-rooted dynamic plans, exact displacements, and union behavior."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile


UNIT = "0123456789abcdef"
TABLE = "fedcba9876543210"


def sidecar(patch: str) -> dict:
    nodes = [
        {"index": 0, "name_hash": "00000001", "parent": None},
        {"index": 1, "name_hash": "c4787b4e", "parent": 0},
        {"index": 2, "name_hash": "00000002", "parent": 1},
        {"index": 3, "name_hash": "00000003", "parent": 1},
        {"index": 4, "name_hash": "00000004", "parent": 3},
        {"index": 5, "name_hash": "b2bcd839", "parent": 0},
        {"index": 6, "name_hash": "00000005", "parent": 5},
        {"index": 7, "name_hash": "00000006", "parent": 6},
    ]
    return {
        "schema": "HD2RIG", "version": 1,
        "source": {
            "patch": patch, "unit_id": UNIT, "unit_sha256": "11" * 32,
            "triplet": {"main": {"sha256": "22" * 32}},
        },
        "roles": {
            "left_shoulder": {"status": "resolved", "node": 3},
            "right_shoulder": {"status": "resolved", "node": 6},
        },
        "scene_graph": {"nodes": nodes},
        "lods": [{
            "lod": 0, "entry_count": 8,
            "slot_to_scene_node": list(range(8)),
            "table": {"t48_fingerprint_fnv1a": TABLE},
        }],
    }


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[1]
    tool = root / "tools" / "patch_profile_tool.py"
    sys.path.insert(0, str(root / "tools"))
    from pose_branch_plan import build_pose_branch_plan

    with tempfile.TemporaryDirectory() as temporary:
        directory = pathlib.Path(temporary)
        rigs = directory / "rigs"
        rigs.mkdir()
        for index in range(2):
            (rigs / f"copy{index}.hd2rig.json").write_text(
                json.dumps(sidecar(f"copy{index}.patch_0")), encoding="utf-8")
        output = directory / "pose_shoulder_plan.json"
        active = {(int(UNIT, 16), int(TABLE, 16), 8)}
        plan = build_pose_branch_plan(
            str(rigs), str(output), (0.06, 0.0, 0.0), (-0.06, 0.0, 0.0), active, False)

        if plan["table_count"] != 1 or plan["slot_count"] != 7:
            raise AssertionError("redundant sidecars were not unioned into one seven-slot table")
        slots = plan["tables"][0]["slots"]
        if {slot["slot"] for slot in slots if slot["side"] == "left"} != {1, 2, 3, 4}:
            raise AssertionError("the full left clavicle subtree, including its helper, was not selected")
        if {slot["slot"] for slot in slots if slot["side"] == "right"} != {5, 6, 7}:
            raise AssertionError("the full right clavicle subtree was not selected")
        for slot in slots:
            expected = [0.06, 0.0, 0.0] if slot["side"] == "left" else [-0.06, 0.0, 0.0]
            if slot["output_displacement_m"] != expected:
                raise AssertionError("a descendant received a tapered or side-incorrect displacement")
            if len(slot["sources"]) != 2:
                raise AssertionError("duplicate semantic sources were not retained as provenance")
        if plan["falloff"] is not None or plan["runtime_status"] != "offline_plan_only":
            raise AssertionError("the plan does not declare its no-falloff/readiness boundary")

        rejected = subprocess.run(
            [sys.executable, str(tool), "pose-branch-plan", "--rig-dir", str(rigs),
             "--profile-dir", str(directory / "missing"), "--out", str(directory / "bad.json")],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        if rejected.returncode != 2 or "active_profiles.txt" not in rejected.stdout:
            raise AssertionError("the CLI accepted a plan without an active runtime registry")

    print("clavicle-rooted pose branch plan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
