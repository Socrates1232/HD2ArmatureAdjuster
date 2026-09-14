#!/usr/bin/env python
"""Build a JSON-only A/B evidence report from two HD2 patch bundles.

Runtime packages are produced by extract_runtime_profile.py. This historical
validator intentionally does not emit C++ headers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct


BUNDLE_HEADER_SIZE = 72
TYPE_ENTRY_SIZE = 32
FILE_ENTRY_SIZE = 80


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_file(path: str) -> bytes:
    with open(path, "rb") as stream:
        return stream.read()


def find_unit(bundle: bytes, unit_id: int) -> tuple[bytes, int]:
    if len(bundle) < BUNDLE_HEADER_SIZE:
        raise ValueError("bundle header is truncated")
    magic, type_count, file_count, _ = struct.unpack_from("<4I", bundle, 0)
    if magic != 0xF0000011:
        raise ValueError("not an HD2 patch bundle")
    entries = BUNDLE_HEADER_SIZE + type_count * TYPE_ENTRY_SIZE
    if entries + file_count * FILE_ENTRY_SIZE > len(bundle):
        raise ValueError("bundle file table is truncated")
    for index in range(file_count):
        entry = entries + index * FILE_ENTRY_SIZE
        file_id, _, data_offset = struct.unpack_from("<3Q", bundle, entry)
        data_size = struct.unpack_from("<I", bundle, entry + 0x38)[0]
        if file_id == unit_id:
            end = data_offset + data_size
            if end > len(bundle):
                raise ValueError("unit payload is truncated")
            return bundle[data_offset:end], data_offset
    raise ValueError(f"unit {unit_id:016x} is not in the bundle")


def inverse_bind_tables(unit: bytes) -> list[dict]:
    if len(unit) < 0x5C:
        raise ValueError("unit header is truncated")
    bone_info = struct.unpack_from("<I", unit, 0x58)[0]
    if bone_info + 4 > len(unit):
        raise ValueError("BoneInfo offset is outside the unit")
    count = struct.unpack_from("<I", unit, bone_info)[0]
    if bone_info + 4 + 4 * count > len(unit):
        raise ValueError("BoneInfo LOD list is truncated")
    tables = []
    for lod in range(count):
        relative = struct.unpack_from("<I", unit, bone_info + 4 + 4 * lod)[0]
        block = bone_info + relative
        if block + 16 > len(unit):
            raise ValueError(f"LOD {lod} BoneInfo block is truncated")
        bones, matrix_offset, _, _ = struct.unpack_from("<4I", unit, block)
        start = block + matrix_offset
        end = start + bones * 64
        if end > len(unit):
            raise ValueError(f"LOD {lod} inverse-bind table is truncated")
        tables.append({"lod": lod, "bones": bones, "offset": start,
                       "file64": unit[start:end]})
    return tables


def file64_to_t48(table: bytes) -> bytes:
    if len(table) % 64:
        raise ValueError("file64 table size is not divisible by 64")
    output = bytearray()
    for offset in range(0, len(table), 64):
        matrix = struct.unpack_from("<16f", table, offset)
        packed = (matrix[0], matrix[4], matrix[8], matrix[12],
                  matrix[1], matrix[5], matrix[9], matrix[13],
                  matrix[2], matrix[6], matrix[10], matrix[14])
        output += struct.pack("<12f", *packed)
    return bytes(output)


def translate_world_file64(
        matrix: bytes,
        translation: tuple[float, float, float],
) -> tuple[bytes, tuple[float, float, float]]:
    if len(matrix) != 64:
        raise ValueError("an inverse-bind matrix must be 64 bytes")
    values = list(struct.unpack("<16f", matrix))
    delta = tuple(sum(translation[row] * values[row * 4 + column]
                      for row in range(3)) for column in range(3))
    for column in range(3):
        values[12 + column] += delta[column]
    return struct.pack("<16f", *values), delta


def triplet_hashes(main_path: str) -> dict:
    result = {}
    for name, path in (("main", main_path),
                       ("gpu", main_path + ".gpu_resources"),
                       ("stream", main_path + ".stream")):
        if os.path.isfile(path):
            data = read_file(path)
            result[name] = {"bytes": len(data), "sha256": sha256(data)}
    return result


def build(a_path: str, b_path: str, unit_id: int, slot: int, profile_lod: int) -> tuple[dict, bytes, bytes, bytes, bytes]:
    a_bundle = read_file(a_path)
    b_bundle = read_file(b_path)
    if len(a_bundle) != len(b_bundle):
        raise ValueError("A/B main bundles differ in size; use a no-growth control")
    a_unit, a_base = find_unit(a_bundle, unit_id)
    b_unit, b_base = find_unit(b_bundle, unit_id)
    if a_base != b_base or len(a_unit) != len(b_unit):
        raise ValueError("A/B unit placement differs")
    a_tables = inverse_bind_tables(a_unit)
    b_tables = inverse_bind_tables(b_unit)
    if [(x["lod"], x["bones"], x["offset"]) for x in a_tables] != [
            (x["lod"], x["bones"], x["offset"]) for x in b_tables]:
        raise ValueError("A/B BoneInfo layouts differ")
    if profile_lod >= len(a_tables):
        raise ValueError("profile LOD is outside the unit")

    allowed = set()
    lod_records = []
    for a, b in zip(a_tables, b_tables):
        a_t48 = file64_to_t48(a["file64"])
        b_t48 = file64_to_t48(b["file64"])
        record = {
            "lod": a["lod"], "bones": a["bones"],
            "a_file64_sha256": sha256(a["file64"]),
            "b_file64_sha256": sha256(b["file64"]),
            "a_t48_sha256": sha256(a_t48),
            "b_t48_sha256": sha256(b_t48),
        }
        if slot < a["bones"]:
            row = a["offset"] + slot * 64 + 48
            allowed.update(range(a_base + row, a_base + row + 16))
            record.update({
                "a_slot_file64": a["file64"][slot * 64:(slot + 1) * 64].hex(),
                "b_slot_file64": b["file64"][slot * 64:(slot + 1) * 64].hex(),
                "a_slot_t48": a_t48[slot * 48:(slot + 1) * 48].hex(),
                "b_slot_t48": b_t48[slot * 48:(slot + 1) * 48].hex(),
            })
        lod_records.append(record)

    differences = [index for index, pair in enumerate(zip(a_bundle, b_bundle))
                   if pair[0] != pair[1]]
    unexpected = [index for index in differences if index not in allowed]
    if unexpected:
        raise ValueError(f"A/B bundles differ outside target translation rows at {unexpected[0]:#x}")
    if not differences:
        raise ValueError("A and B are byte-identical")

    a_gpu = read_file(a_path + ".gpu_resources")
    b_gpu = read_file(b_path + ".gpu_resources")
    a_stream = read_file(a_path + ".stream")
    b_stream = read_file(b_path + ".stream")
    if a_gpu != b_gpu or a_stream != b_stream:
        raise ValueError("A/B GPU or stream resources differ")

    chosen_a = a_tables[profile_lod]
    chosen_b = b_tables[profile_lod]
    if slot >= chosen_a["bones"]:
        raise ValueError(f"slot {slot} is outside profile LOD {profile_lod}")
    chosen_a_t48 = file64_to_t48(chosen_a["file64"])
    chosen_b_t48 = file64_to_t48(chosen_b["file64"])
    profile = {
        "schema": 1,
        "unit_id": f"{unit_id:016x}",
        "slot": slot,
        "profile_lod": profile_lod,
        "a": {"file": os.path.basename(a_path), "triplet": triplet_hashes(a_path)},
        "b": {"file": os.path.basename(b_path), "triplet": triplet_hashes(b_path)},
        "main_bytes_changed": len(differences),
        "changed_bytes_are_only_target_translation_rows": True,
        "gpu_and_stream_identical": True,
        "lods": lod_records,
    }
    a_slot = chosen_a["file64"][slot * 64:(slot + 1) * 64]
    b_slot = chosen_b["file64"][slot * 64:(slot + 1) * 64]
    return profile, chosen_a_t48, chosen_b_t48, a_slot, b_slot


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True, help="original/no-op .patch_N")
    parser.add_argument("--b", required=True, help="single-slot displaced .patch_N")
    parser.add_argument("--unit", required=True, help="16-digit unit file ID")
    parser.add_argument("--slot", required=True, type=int)
    parser.add_argument("--lod", type=int, default=0)
    parser.add_argument("--out", required=True, help="output JSON profile")
    args = parser.parse_args()

    profile, a_t48, b_t48, a_slot, b_slot = build(
        args.a, args.b, int(args.unit, 16), args.slot, args.lod)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(profile, stream, indent=2)
        stream.write("\n")
    print(json.dumps({"unit": profile["unit_id"], "slot": profile["slot"],
                      "lod": profile["profile_lod"],
                      "main_bytes_changed": profile["main_bytes_changed"],
                      "t48_bytes": len(a_t48)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
