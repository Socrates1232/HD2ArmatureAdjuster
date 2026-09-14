#!/usr/bin/env python
"""Verify semantic sidecars, complete branches, and patch immutability."""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile

from branch_target_pipeline_test import UNIT_ID, murmur64_high, synthetic_bundle


DATA_OFFSET = 72 + 32 + 80
SCENE_OFFSET = 0x68
NODE_COUNT = 8
PARENT_OFFSET = DATA_OFFSET + SCENE_OFFSET + 16 + NODE_COUNT * 128
HASH_OFFSET = PARENT_OFFSET + NODE_COUNT * 4


def semantic_bundle() -> bytes:
    data = bytearray(synthetic_bundle(False))
    names = ["spine_2", "l_shoulder", "l_elbow", "l_hand",
             "r_shoulder", "r_elbow", "r_hand", "l_finger"]
    struct.pack_into(f"<{NODE_COUNT}I", data, HASH_OFFSET,
                     *(murmur64_high(name) for name in names))
    struct.pack_into("<HH", data, PARENT_OFFSET + 6 * 4, 1, 5)
    return bytes(data)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_tool(tool: pathlib.Path, root: pathlib.Path, output: pathlib.Path,
             roles: bool = True) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(tool), "rig-sidecars", "--root", str(root),
               "--out-dir", str(output)]
    if roles:
        for assignment in ("chest=spine_2", "left_elbow=l_elbow", "right_elbow=r_elbow",
                           "left_hand=l_hand", "right_hand=r_hand"):
            command += ["--role", assignment]
    return subprocess.run(command, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, check=False)


def expect_rejected(tool: pathlib.Path, parent: pathlib.Path, name: str,
                    data: bytes, message: str) -> None:
    root = parent / name
    root.mkdir()
    (root / "0123456789abcdef.patch_0").write_bytes(data)
    completed = run_tool(tool, root, root / "profiles", roles=False)
    if completed.returncode != 2 or message not in completed.stdout:
        raise AssertionError(f"{name} was not rejected as {message!r}: {completed.stdout}")


def main() -> int:
    tool = pathlib.Path(__file__).resolve().parents[1] / "tools" / "patch_profile_tool.py"
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary) / "positive"
        root.mkdir()
        patch = root / "0123456789abcdef.patch_0"
        patch.write_bytes(semantic_bundle())
        (root / (patch.name + ".gpu_resources")).write_bytes(b"gpu-sidecar-fixture")
        (root / (patch.name + ".stream")).write_bytes(b"stream-sidecar-fixture")
        before = {path.name: digest(path) for path in root.iterdir() if path.is_file()}

        output = root / "HD2ArmatureProfiles"
        completed = run_tool(tool, root, output)
        if completed.returncode:
            raise AssertionError(completed.stdout)
        sidecars = list(output.glob("*.hd2rig.json"))
        if len(sidecars) != 1:
            raise AssertionError(f"expected one unit sidecar, found {len(sidecars)}")
        document = json.loads(sidecars[0].read_text(encoding="utf-8"))
        if document["schema"] != "HD2RIG" or document["version"] != 1:
            raise AssertionError("sidecar schema/version is absent")
        if document["source"]["unit_id"] != f"{UNIT_ID:016x}":
            raise AssertionError("unit dependency was not recorded")
        if set(document["source"]["triplet"]) != {"main", "gpu", "stream"}:
            raise AssertionError("exact patch triplet dependencies were not recorded")
        if any(item["status"] != "resolved" for item in document["roles"].values()):
            raise AssertionError("configured chest/shoulder/elbow/hand roles were not resolved")
        nodes = document["scene_graph"]["nodes"]
        if len(nodes) != NODE_COUNT or "raw_hex" not in nodes[2]["local_transform"]:
            raise AssertionError("raw local TransformData was not retained")
        if len(nodes[2]["stored_rest_matrix"]) != 16:
            raise AssertionError("stored rest matrices were not exported")
        if len(document["complete_shoulder_descendants"]["left"]) != 4:
            raise AssertionError("unweighted traversal did not retain the full left branch")
        for lod in document["lods"]:
            left = lod["shoulder_branches"]["left"]
            right = lod["shoulder_branches"]["right"]
            if len(left) != 4 or len(right) != 3:
                raise AssertionError("per-LOD complete weighted branches were not exported")
            if max(item["depth"] for item in left) != 3:
                raise AssertionError("the new semantic branch was incorrectly depth-tapered")
            if len(lod["slot_to_scene_node"]) != lod["entry_count"]:
                raise AssertionError("slot-to-node mapping is incomplete")
            if not lod["table"]["t48_fingerprint_fnv1a"]:
                raise AssertionError("runtime table identity is absent")

        after = {path.name: digest(path) for path in root.iterdir() if path.is_file()}
        if before != after:
            raise AssertionError("sidecar export changed source patch triplet bytes")

        cycle = bytearray(semantic_bundle())
        struct.pack_into("<HH", cycle, PARENT_OFFSET + 2 * 4, 1, 3)
        expect_rejected(tool, pathlib.Path(temporary), "cycle", cycle, "parent cycle")

        out_of_range = bytearray(semantic_bundle())
        struct.pack_into("<HH", out_of_range, PARENT_OFFSET + 2 * 4, 1, NODE_COUNT)
        expect_rejected(tool, pathlib.Path(temporary), "range", out_of_range,
                        "out-of-range parent")

        ambiguous = bytearray(semantic_bundle())
        struct.pack_into("<I", ambiguous, HASH_OFFSET + 7 * 4,
                         murmur64_high("l_shoulder"))
        expect_rejected(tool, pathlib.Path(temporary), "ambiguous", ambiguous,
                        "matches multiple scene nodes")

    print("semantic rig sidecars and patch immutability passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
