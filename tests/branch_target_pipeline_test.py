#!/usr/bin/env python
"""Verify table-qualified shoulder branch extraction and ambiguity rejection."""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile


UNIT_ID = 0x0123456789ABCDEF
UNIT_TYPE = 0xE0A48D0BE9A7453F


def murmur64_high(name: str) -> int:
    data = name.encode("utf-8")
    multiplier = 0xC6A4A7935BD1E995
    mask = 0xFFFFFFFFFFFFFFFF
    value = (len(data) * multiplier) & mask
    whole = len(data) // 8
    for index in range(whole):
        item = struct.unpack_from("<Q", data, index * 8)[0]
        item = (item * multiplier) & mask
        item ^= item >> 47
        item = (item * multiplier) & mask
        value ^= item
        value = (value * multiplier) & mask
    tail = data[whole * 8:]
    for index in range(len(tail) - 1, -1, -1):
        value ^= tail[index] << (8 * index)
    if tail:
        value = (value * multiplier) & mask
    value ^= value >> 47
    value = (value * multiplier) & mask
    value ^= value >> 47
    return value >> 32


def matrix(seed: float) -> bytes:
    return struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                       0, 0, 1, 0, seed, seed * 2, seed * 3, 1)


def bone_block(real: list[int], seed: float) -> bytes:
    count = len(real)
    matrix_offset = 16
    real_offset = matrix_offset + 64 * count
    figure_offset = real_offset + 4 * count
    return (struct.pack("<4I", count, matrix_offset, real_offset, figure_offset) +
            b"".join(matrix(seed + index / 10) for index in range(count)) +
            struct.pack(f"<{count}I", *real) + struct.pack("<I", 0))


def synthetic_bundle(ambiguous: bool) -> bytes:
    parents = [(0, 0), (1, 0), (1, 1), (1, 2),
               (1, 0), (1, 4), (1, 0), (1, 3)]
    hashes = [0x100, murmur64_high("l_shoulder"), 0x102, 0x103,
              murmur64_high("r_shoulder"), 0x105, 0x106, 0x107]
    count = len(parents)
    scene = (struct.pack("<4I", count, 0, 0, 0) +
             b"".join(matrix(0) for _ in range(count)) +
             b"".join(matrix(index / 10) for index in range(count)) +
             b"".join(struct.pack("<HH", *parent) for parent in parents) +
             struct.pack(f"<{count}I", *hashes))

    lod0 = [0, 1, 2, 3, 7, 4, 5, 6]
    lod1 = [0, 6, 1, 2, 3, 7, 4, 5]
    first = bone_block(lod0, 1.0)
    second = bone_block(lod1, 1.0 if ambiguous else 10.0)
    bone_info = struct.pack("<3I", 2, 12, 12 + len(first)) + first + second

    scene_offset = 0x68
    bone_offset = scene_offset + len(scene)
    unit = bytearray(scene_offset)
    struct.pack_into("<I", unit, 0x34, scene_offset)
    struct.pack_into("<I", unit, 0x58, bone_offset)
    unit += scene + bone_info

    data_offset = 72 + 32 + 80
    bundle = bytearray(data_offset)
    struct.pack_into("<4I", bundle, 0, 0xF0000011, 1, 1, 0)
    struct.pack_into("<QQIIII", bundle, 72, 0, UNIT_TYPE, 1, 0, 0x10, 0x40)
    struct.pack_into("<QQQQQQQIIIIII", bundle, 104, UNIT_ID, UNIT_TYPE,
                     data_offset, 0, 0, 0, 0, len(unit), 0, 0, 0x10, 0x40, 0)
    return bytes(bundle) + unit


def run_tool(tool: pathlib.Path, root: pathlib.Path) -> subprocess.CompletedProcess[str]:
    profiles = root / "HD2ArmatureProfiles"
    profile = profiles / "synthetic.hd2profile"
    profiles.mkdir()
    extract = tool.parent / "extract_runtime_profile.py"
    generated = subprocess.run(
        [sys.executable, str(extract), "--patch", str(root / "0123456789abcdef.patch_0"),
         "--out", str(profile)], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False)
    if generated.returncode:
        raise AssertionError(generated.stdout)
    (profiles / "active_profiles.txt").write_text(profile.name + "\n", encoding="utf-8")
    return subprocess.run(
        [sys.executable, str(tool), "shoulder-targets", "--root", str(root),
         "--profile-dir", str(profiles)], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False)


def main() -> int:
    tool = pathlib.Path(__file__).resolve().parents[1] / "tools" / "patch_profile_tool.py"
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary) / "positive"
        root.mkdir()
        (root / "0123456789abcdef.patch_0").write_bytes(synthetic_bundle(False))
        completed = run_tool(tool, root)
        if completed.returncode:
            raise AssertionError(completed.stdout)
        rows = [line.split() for line in
                (root / "HD2ArmatureProfiles" / "shoulder_targets.txt").read_text(
                    encoding="utf-8").splitlines() if line and not line.startswith("#")]
        if len(rows) != 10 or any(len(row) != 6 for row in rows):
            raise AssertionError("branch generator did not emit ten table-qualified targets")
        if len({row[1] for row in rows}) != 2:
            raise AssertionError("different LOD tables were not kept semantically distinct")
        if {tuple(row[3:]) for row in rows} != {
                ("+0.03", "+0", "+0"), ("+0.02", "+0", "+0"),
                ("+0.01", "+0", "+0"), ("-0.03", "+0", "+0"),
                ("-0.02", "+0", "+0")}:
            raise AssertionError("branch falloff translations were not assigned correctly")
        report = (root / "HD2ArmatureProfiles" / "shoulder_targets.json").read_text(
            encoding="utf-8")
        if '"falloff_depth": 3' not in report or report.count('"depth": 3') < 2:
            raise AssertionError("distal descendant exclusion was not reported")

        conflict = pathlib.Path(temporary) / "conflict"
        conflict.mkdir()
        (conflict / "0123456789abcdef.patch_0").write_bytes(synthetic_bundle(True))
        rejected = run_tool(tool, conflict)
        if rejected.returncode != 2 or "conflicting branch semantics" not in rejected.stdout:
            raise AssertionError("ambiguous identical runtime tables were not rejected")

    print("table-qualified descendant extraction and ambiguity rejection passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
