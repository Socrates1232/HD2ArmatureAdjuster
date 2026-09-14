#!/usr/bin/env python
"""Extract runtime inverse-bind profiles from one finished HD2 patch bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import tempfile

from build_ib_profile import file64_to_t48, inverse_bind_tables, read_file, sha256


BUNDLE_HEADER_SIZE = 72
TYPE_ENTRY_SIZE = 32
FILE_ENTRY_SIZE = 80
UNIT_TYPE = 0xE0A48D0BE9A7453F
MAGIC = b"HD2IBP1\0"
HEADER_SIZE = 64
RECORD_HEADER_SIZE = 40


def fnv1a(data: bytes) -> int:
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def bundle_entries(bundle: bytes) -> list[dict]:
    if len(bundle) < BUNDLE_HEADER_SIZE:
        raise ValueError("bundle header is truncated")
    magic, type_count, file_count, _ = struct.unpack_from("<4I", bundle, 0)
    if magic != 0xF0000011:
        raise ValueError("not an HD2 patch bundle")
    entries_offset = BUNDLE_HEADER_SIZE + type_count * TYPE_ENTRY_SIZE
    if entries_offset + file_count * FILE_ENTRY_SIZE > len(bundle):
        raise ValueError("bundle file table is truncated")
    entries = []
    for index in range(file_count):
        offset = entries_offset + index * FILE_ENTRY_SIZE
        fields = struct.unpack_from("<QQQQQQQIIIIII", bundle, offset)
        end = fields[2] + fields[7]
        if end > len(bundle):
            raise ValueError(f"resource {fields[0]:016x} is truncated")
        entries.append({
            "file_id": fields[0],
            "type_id": fields[1],
            "data_offset": fields[2],
            "data_size": fields[7],
        })
    return entries


def triplet_metadata(patch_path: str) -> dict:
    result = {}
    for kind, path in (("main", patch_path),
                       ("gpu", patch_path + ".gpu_resources"),
                       ("stream", patch_path + ".stream")):
        if os.path.isfile(path):
            data = read_file(path)
            result[kind] = {"bytes": len(data), "sha256": sha256(data)}
    return result


def extract(patch_path: str, requested_units: set[int] | None) -> tuple[list[dict], list[dict]]:
    bundle = read_file(patch_path)
    records = []
    skipped = []
    seen_units = set()
    for entry in bundle_entries(bundle):
        if entry["type_id"] != UNIT_TYPE:
            continue
        unit_id = entry["file_id"]
        if requested_units is not None and unit_id not in requested_units:
            continue
        seen_units.add(unit_id)
        unit = bundle[entry["data_offset"]:entry["data_offset"] + entry["data_size"]]
        try:
            tables = inverse_bind_tables(unit)
        except (ValueError, struct.error) as error:
            skipped.append({"unit_id": f"{unit_id:016x}", "reason": str(error)})
            if requested_units is not None:
                raise ValueError(f"unit {unit_id:016x}: {error}") from error
            continue

        unique = {}
        for table in tables:
            if table["lod"] >= 32:
                raise ValueError(f"unit {unit_id:016x} has LOD {table['lod']} outside the profile mask")
            if table["bones"] == 0 or table["bones"] > 4096:
                raise ValueError(f"unit {unit_id:016x} LOD {table['lod']} has invalid bone count")
            t48 = file64_to_t48(table["file64"])
            existing = unique.get(t48)
            if existing is None:
                unique[t48] = {
                    "unit_id": unit_id,
                    "lod_mask": 1 << table["lod"],
                    "first_lod": table["lod"],
                    "entries": table["bones"],
                    "anchor_slot": 0,
                    "t48": t48,
                }
            else:
                existing["lod_mask"] |= 1 << table["lod"]
        records.extend(unique.values())

    if requested_units is not None:
        missing = requested_units - seen_units
        if missing:
            raise ValueError("requested unit(s) not found: " + ", ".join(f"{value:016x}" for value in sorted(missing)))
    records.sort(key=lambda item: (item["unit_id"], item["first_lod"], item["entries"]))
    if not records:
        raise ValueError("no inverse-bind tables were extracted")
    return records, skipped


def encode(patch_path: str, records: list[dict]) -> bytes:
    patch_name = os.path.basename(patch_path).encode("utf-8")
    if not patch_name or len(patch_name) > 1024:
        raise ValueError("patch basename length is outside the profile format")
    payload = bytearray(patch_name)
    for record in records:
        table = record["t48"]
        payload += struct.pack(
            "<QIIIIQII",
            record["unit_id"],
            record["lod_mask"],
            record["entries"],
            record["anchor_slot"],
            len(table),
            fnv1a(table),
            record["first_lod"],
            0,
        )
        payload += table
    patch_hash = hashlib.sha256(read_file(patch_path)).digest()
    header = struct.pack(
        "<8sIIII32sQ",
        MAGIC,
        HEADER_SIZE,
        len(records),
        len(patch_name),
        0,
        patch_hash,
        fnv1a(payload),
    )
    if len(header) != HEADER_SIZE:
        raise AssertionError("profile header layout changed")
    return header + payload


def write_atomic(path: str, data: bytes) -> None:
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    handle, temporary = tempfile.mkstemp(prefix=".hd2profile-", dir=directory)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    except BaseException:
        os.unlink(temporary)
        raise


def generate_profile(patch_path: str, output_path: str,
                     requested_units: set[int] | None = None,
                     manifest_path: str | None = None) -> dict:
    records, skipped = extract(patch_path, requested_units)
    encoded = encode(patch_path, records)
    manifest = {
        "schema": 1,
        "format": "HD2IBP1",
        "patch": os.path.basename(patch_path),
        "triplet": triplet_metadata(patch_path),
        "profile": {
            "file": os.path.basename(output_path),
            "bytes": len(encoded),
            "sha256": sha256(encoded),
            "records": len(records),
        },
        "tables": [{
            "unit_id": f"{record['unit_id']:016x}",
            "lod_mask": f"0x{record['lod_mask']:08x}",
            "first_lod": record["first_lod"],
            "entries": record["entries"],
            "anchor_slot": record["anchor_slot"],
            "t48_bytes": len(record["t48"]),
            "t48_sha256": sha256(record["t48"]),
            "t48_fnv1a": f"{fnv1a(record['t48']):016x}",
        } for record in records],
        "skipped_units": skipped,
    }
    write_atomic(output_path, encoded)
    write_atomic(manifest_path or output_path + ".json",
                 (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patch", required=True, help="finished .patch_N main file")
    parser.add_argument("--unit", action="append", default=[], help="optional 16-digit unit ID; repeatable")
    parser.add_argument("--out", required=True, help="output .hd2profile file")
    parser.add_argument("--manifest", help="reviewable JSON output; default is <out>.json")
    args = parser.parse_args()

    requested = {int(value, 16) for value in args.unit} if args.unit else None
    manifest_path = args.manifest or args.out + ".json"
    manifest = generate_profile(args.patch, args.out, requested, manifest_path)
    print(json.dumps({
        "profile": os.path.abspath(args.out),
        "manifest": os.path.abspath(manifest_path),
        "patch": manifest["patch"],
        "records": manifest["profile"]["records"],
        "units": len({record["unit_id"] for record in manifest["tables"]}),
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
