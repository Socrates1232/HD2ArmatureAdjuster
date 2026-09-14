#!/usr/bin/env python
"""End-to-end test for the patch-to-runtime-profile extractor."""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


UNIT_ID = 0x0123456789ABCDEF
UNIT_TYPE = 0xE0A48D0BE9A7453F


def matrix(seed: float) -> bytes:
    values = [
        1.0 + seed, 0.1, 0.2, 0.0,
        0.3, 1.0 + seed, 0.4, 0.0,
        0.5, 0.6, 1.0 + seed, 0.0,
        seed, seed * 2.0, seed * 3.0, 1.0,
    ]
    return struct.pack("<16f", *values)


def synthetic_bundle() -> bytes:
    table = matrix(0.25) + matrix(0.5)
    block = struct.pack("<4I", 2, 16, 0, 0) + table
    bone_info = struct.pack("<3I", 2, 12, 12 + len(block)) + block + block
    unit = bytearray(0x60)
    struct.pack_into("<I", unit, 0x58, 0x60)
    unit += bone_info

    data_offset = 72 + 32 + 80
    bundle = bytearray(data_offset)
    struct.pack_into("<4I", bundle, 0, 0xF0000011, 1, 1, 0)
    struct.pack_into("<QQIIII", bundle, 72, 0, UNIT_TYPE, 1, 0, 0x10, 0x40)
    struct.pack_into(
        "<QQQQQQQIIIIII",
        bundle,
        104,
        UNIT_ID,
        UNIT_TYPE,
        data_offset,
        0,
        0,
        0,
        0,
        len(unit),
        0,
        0,
        0x10,
        0x40,
        0,
    )
    return bytes(bundle) + unit


def main() -> int:
    tools = pathlib.Path(__file__).resolve().parents[1] / "tools"
    extractor = tools / "extract_runtime_profile.py"
    editor = tools / "patch_profile_tool.py"
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        patch = root / "0123456789abcdef.patch_3"
        profile = root / "example.hd2profile"
        patch.write_bytes(synthetic_bundle())
        pathlib.Path(str(patch) + ".gpu_resources").write_bytes(b"synthetic gpu")
        pathlib.Path(str(patch) + ".stream").write_bytes(b"synthetic stream")
        completed = subprocess.run(
            [
                sys.executable,
                str(extractor),
                "--patch",
                str(patch),
                "--unit",
                f"{UNIT_ID:016x}",
                "--out",
                str(profile),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if completed.returncode:
            raise AssertionError(completed.stdout)

        data = profile.read_bytes()
        manifest = json.loads(pathlib.Path(str(profile) + ".json").read_text(encoding="utf-8"))
        if data[:8] != b"HD2IBP1\0" or struct.unpack_from("<I", data, 12)[0] != 1:
            raise AssertionError("binary profile header is incorrect")
        if manifest["patch"] != patch.name or manifest["profile"]["records"] != 1:
            raise AssertionError("manifest identity is incorrect")
        table = manifest["tables"][0]
        if table["unit_id"] != f"{UNIT_ID:016x}" or table["lod_mask"] != "0x00000003":
            raise AssertionError("identical LOD tables were not deduplicated")
        if manifest["triplet"]["main"]["sha256"] != hashlib.sha256(patch.read_bytes()).hexdigest():
            raise AssertionError("patch hash is incorrect")

        inspected = subprocess.run(
            [sys.executable, str(editor), "inspect", "--patch", str(patch)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if inspected.returncode:
            raise AssertionError(inspected.stdout)
        inventory = json.loads(inspected.stdout)
        if inventory["units"][0]["unit_id"] != f"{UNIT_ID:016x}":
            raise AssertionError("editor inspection missed the synthetic unit")
        if [item["slots"] for item in inventory["units"][0]["lods"]] != [2, 2]:
            raise AssertionError("editor inspection reported incorrect slot counts")

        output_directory = root / "edited"
        output_directory.mkdir()
        output = output_directory / patch.name
        translation = (0.2, -0.3, 0.4)
        edited = subprocess.run(
            [
                sys.executable,
                str(editor),
                "translate",
                "--patch", str(patch),
                "--out-patch", str(output),
                "--unit", f"{UNIT_ID:016x}",
                "--slot", "1",
                "--translate", *(str(value) for value in translation),
            ],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if edited.returncode:
            raise AssertionError(edited.stdout)

        source_bytes = patch.read_bytes()
        output_bytes = output.read_bytes()
        if len(source_bytes) != len(output_bytes):
            raise AssertionError("editor changed the main-patch size")
        if pathlib.Path(str(output) + ".gpu_resources").read_bytes() != b"synthetic gpu":
            raise AssertionError("editor changed the GPU companion")
        if pathlib.Path(str(output) + ".stream").read_bytes() != b"synthetic stream":
            raise AssertionError("editor changed the stream companion")

        # The synthetic unit begins at 184. Its slot-1 matrices begin at unit
        # offsets 188 and 332 in LODs 0 and 1.
        matrix_offsets = (184 + 188, 184 + 332)
        allowed = {offset + byte
                   for offset in matrix_offsets
                   for byte in range(48, 60)}
        changed = {index for index, values in enumerate(zip(source_bytes, output_bytes))
                   if values[0] != values[1]}
        if not changed or not changed.issubset(allowed):
            raise AssertionError("editor changed bytes outside the selected translation rows")
        for offset in matrix_offsets:
            before = struct.unpack_from("<16f", source_bytes, offset)
            after = struct.unpack_from("<16f", output_bytes, offset)
            delta = tuple(sum(translation[row] * before[row * 4 + column]
                              for row in range(3)) for column in range(3))
            expected = tuple(before[12 + column] + delta[column]
                             for column in range(3))
            if before[:12] != after[:12] or any(
                    abs(after[12 + column] - expected[column]) > 1e-6
                    for column in range(3)):
                raise AssertionError("editor applied the wrong inverse-bind translation")

        output_profile = pathlib.Path(str(output) + ".hd2profile")
        output_manifest = json.loads(pathlib.Path(str(output_profile) + ".json").read_text(
            encoding="utf-8"))
        report = json.loads(pathlib.Path(str(output) + ".edit.json").read_text(
            encoding="utf-8"))
        if output_manifest["patch"] != output.name or output_manifest["profile"]["records"] != 1:
            raise AssertionError("editor did not generate the matching runtime profile")
        if [item["lod"] for item in report["mutations"]] != [0, 1]:
            raise AssertionError("editor did not update every applicable LOD")
        if not report["changes_only_target_translation_rows"]:
            raise AssertionError("editor report did not record the edit invariant")

    print("profile extraction, patch inspection, verified translation, and regeneration passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
