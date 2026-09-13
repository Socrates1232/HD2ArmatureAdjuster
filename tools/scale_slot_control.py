#!/usr/bin/env python
"""Scale an already validated no-growth single-slot A/B control."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import struct

from build_ib_profile import find_unit, inverse_bind_tables, read_file, sha256


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True, help="validated displaced control")
    parser.add_argument("--unit", required=True)
    parser.add_argument("--slot", required=True, type=int)
    parser.add_argument("--factor", required=True, type=float)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    a_bundle = read_file(args.a)
    b_bundle = read_file(args.b)
    if len(a_bundle) != len(b_bundle):
        raise ValueError("A/B bundles differ in size")

    unit_id = int(args.unit, 16)
    a_unit, a_base = find_unit(a_bundle, unit_id)
    b_unit, b_base = find_unit(b_bundle, unit_id)
    if a_base != b_base or len(a_unit) != len(b_unit):
        raise ValueError("A/B unit placement differs")
    a_tables = inverse_bind_tables(a_unit)
    b_tables = inverse_bind_tables(b_unit)
    if [(x["bones"], x["offset"]) for x in a_tables] != [
            (x["bones"], x["offset"]) for x in b_tables]:
        raise ValueError("A/B inverse-bind layouts differ")

    output = bytearray(a_bundle)
    touched = []
    allowed = set()
    for a_table, b_table in zip(a_tables, b_tables):
        if args.slot >= a_table["bones"]:
            if a_table["file64"] != b_table["file64"]:
                raise ValueError(f"LOD {a_table['lod']} changed without the target slot")
            continue
        offset = a_table["offset"] + args.slot * 64
        a_matrix = struct.unpack_from("<16f", a_unit, offset)
        b_matrix = struct.unpack_from("<16f", b_unit, offset)
        for index in range(16):
            if index not in (12, 13, 14) and struct.pack("<f", a_matrix[index]) != struct.pack("<f", b_matrix[index]):
                raise ValueError(f"LOD {a_table['lod']} changes a non-translation component")
        if a_matrix[12:15] == b_matrix[12:15]:
            continue
        scaled = list(a_matrix)
        for index in (12, 13, 14):
            scaled[index] = a_matrix[index] + args.factor * (b_matrix[index] - a_matrix[index])
        absolute = a_base + offset
        struct.pack_into("<16f", output, absolute, *scaled)
        allowed.update(range(absolute + 48, absolute + 60))
        touched.append(a_table["lod"])

    changed = {index for index, (left, right) in enumerate(zip(a_bundle, output)) if left != right}
    if not touched or not changed or not changed.issubset(allowed):
        raise ValueError("output changes are not confined to target translation rows")

    os.makedirs(args.out_dir, exist_ok=True)
    name = os.path.basename(args.a)
    main_path = os.path.join(args.out_dir, name)
    with open(main_path, "wb") as stream:
        stream.write(output)
    for suffix in (".gpu_resources", ".stream"):
        shutil.copyfile(args.a + suffix, main_path + suffix)

    report = {
        "schema": 1,
        "unit_id": f"{unit_id:016x}",
        "slot": args.slot,
        "factor": args.factor,
        "lods_touched": touched,
        "main_bytes": len(output),
        "main_bytes_changed": len(changed),
        "changes_only_target_translation_rows": True,
        "main_sha256": sha256(output),
        "gpu_sha256": sha256(read_file(main_path + ".gpu_resources")),
        "stream_sha256": sha256(read_file(main_path + ".stream")),
    }
    with open(os.path.join(args.out_dir, "scaled_control_report.json"), "w", encoding="utf-8", newline="\n") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
