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
    extractor = pathlib.Path(__file__).resolve().parents[1] / "tools" / "extract_runtime_profile.py"
    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary)
        patch = root / "example.patch_3"
        profile = root / "example.hd2profile"
        patch.write_bytes(synthetic_bundle())
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

    print("patch extraction, LOD deduplication, and manifest generation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
