#!/usr/bin/env python
"""Inspect, translate, and profile HD2 inverse-bind patch resources."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
import tempfile

from build_ib_profile import (inverse_bind_tables, read_file, sha256,
                              translate_world_file64)
from extract_runtime_profile import (UNIT_TYPE, bundle_entries, generate_profile,
                                     write_atomic)


PATCH_NAME = re.compile(r"^[0-9a-fA-F]{16}\.patch_[0-9]+$")
UNIT_ID = re.compile(r"^[0-9a-fA-F]{16}$")


def parse_unit_id(value: str) -> int:
    if not UNIT_ID.fullmatch(value):
        raise argparse.ArgumentTypeError("unit ID must contain exactly 16 hex digits")
    return int(value, 16)


def unit_entries(bundle: bytes) -> list[dict]:
    return [entry for entry in bundle_entries(bundle)
            if entry["type_id"] == UNIT_TYPE]


def inspect_patch(path: str) -> dict:
    bundle = read_file(path)
    units = []
    skipped = []
    for entry in unit_entries(bundle):
        unit_id = entry["file_id"]
        blob = bundle[entry["data_offset"]:
                      entry["data_offset"] + entry["data_size"]]
        try:
            tables = inverse_bind_tables(blob)
            units.append({
                "unit_id": f"{unit_id:016x}",
                "bytes": entry["data_size"],
                "lods": [{
                    "lod": table["lod"],
                    "slots": table["bones"],
                    "file64_sha256": sha256(table["file64"]),
                } for table in tables],
            })
        except (ValueError, struct.error) as error:
            skipped.append({"unit_id": f"{unit_id:016x}", "reason": str(error)})
    return {
        "schema": 1,
        "patch": os.path.basename(path),
        "bytes": len(bundle),
        "sha256": sha256(bundle),
        "units": units,
        "skipped_units": skipped,
    }


def refuse_game_output(path: str) -> None:
    normalized = os.path.abspath(path).replace("\\", "/").lower()
    if "/steamapps/" in normalized or "/helldivers 2/" in normalized:
        raise ValueError("refusing to write generated files directly into a game directory")


def require_new_outputs(paths: list[str], force: bool) -> None:
    if force:
        return
    existing = [path for path in paths if os.path.exists(path)]
    if existing:
        raise ValueError("output already exists; use --force to replace it: " + existing[0])


def edit_patch(source_path: str, output_path: str, profile_path: str,
               report_path: str, unit_id: int, slot: int,
               translation: tuple[float, float, float], lods: list[int] | None,
               force: bool) -> dict:
    source_path = os.path.abspath(source_path)
    output_path = os.path.abspath(output_path)
    profile_path = os.path.abspath(profile_path)
    report_path = os.path.abspath(report_path)
    refuse_game_output(output_path)
    if os.path.normcase(source_path) == os.path.normcase(output_path):
        raise ValueError("source and output patch paths must differ")
    if not PATCH_NAME.fullmatch(os.path.basename(output_path)):
        raise ValueError("--out-patch basename must be 16 hex digits followed by .patch_N")
    if os.path.basename(source_path).lower() != os.path.basename(output_path).lower():
        raise ValueError("output patch basename must match the source archive")
    if slot < 0:
        raise ValueError("slot must be nonnegative")
    if (not all(math.isfinite(value) for value in translation) or
            max(abs(value) for value in translation) > 10.0 or
            not any(value != 0.0 for value in translation)):
        raise ValueError("translation must be finite, nonzero, and within +/-10 metres")

    source_gpu = source_path + ".gpu_resources"
    source_stream = source_path + ".stream"
    for companion in (source_gpu, source_stream):
        if not os.path.isfile(companion):
            raise ValueError("missing patch companion: " + companion)

    final_paths = [output_path, output_path + ".gpu_resources",
                   output_path + ".stream", profile_path,
                   profile_path + ".json", report_path]
    normalized_outputs = {os.path.normcase(os.path.abspath(path))
                          for path in final_paths}
    if len(normalized_outputs) != len(final_paths):
        raise ValueError("output patch, profile, manifest, and report paths must be distinct")
    require_new_outputs(final_paths, force)

    source = read_file(source_path)
    matching = [entry for entry in unit_entries(source)
                if entry["file_id"] == unit_id]
    if len(matching) != 1:
        raise ValueError(f"expected one unit {unit_id:016x}, found {len(matching)}")
    entry = matching[0]
    blob = source[entry["data_offset"]:
                  entry["data_offset"] + entry["data_size"]]
    tables = inverse_bind_tables(blob)
    if not tables:
        raise ValueError("selected unit has no inverse-bind tables")

    requested_lods = None if lods is None else set(lods)
    if requested_lods is not None:
        available = {table["lod"] for table in tables}
        missing = requested_lods - available
        if missing:
            raise ValueError("requested LOD is absent: " +
                             ", ".join(str(value) for value in sorted(missing)))

    output = bytearray(source)
    allowed = set()
    mutations = []
    skipped_lods = []
    for table in tables:
        lod = table["lod"]
        if requested_lods is not None and lod not in requested_lods:
            continue
        if slot >= table["bones"]:
            if requested_lods is not None:
                raise ValueError(f"slot {slot} is outside LOD {lod}'s {table['bones']} slots")
            skipped_lods.append(lod)
            continue
        relative = table["offset"] + slot * 64
        absolute = entry["data_offset"] + relative
        before = bytes(output[absolute:absolute + 64])
        after, bind_delta = translate_world_file64(before, translation)
        output[absolute:absolute + 64] = after
        allowed.update(range(absolute + 48, absolute + 60))
        mutations.append({
            "lod": lod,
            "slot": slot,
            "absolute_offset": absolute,
            "translation_before": list(struct.unpack("<16f", before)[12:15]),
            "translation_after": list(struct.unpack("<16f", after)[12:15]),
            "bind_translation_delta": list(bind_delta),
            "linear_rows_unchanged": before[:48] == after[:48],
        })
    if not mutations:
        raise ValueError("the selected slot is absent from every selected LOD")

    changed = {index for index, pair in enumerate(zip(source, output))
               if pair[0] != pair[1]}
    if not changed:
        raise ValueError("translation rounded to no binary change")
    if len(output) != len(source) or not changed.issubset(allowed):
        raise ValueError("edit escaped the selected inverse-bind translation rows")

    output_directory = os.path.dirname(output_path)
    os.makedirs(output_directory, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".hd2-edit-", dir=output_directory) as staging:
        staged_patch = os.path.join(staging, os.path.basename(output_path))
        staged_profile = os.path.join(staging, os.path.basename(profile_path))
        with open(staged_patch, "wb") as stream:
            stream.write(output)
        with open(staged_patch + ".gpu_resources", "wb") as stream:
            stream.write(read_file(source_gpu))
        with open(staged_patch + ".stream", "wb") as stream:
            stream.write(read_file(source_stream))
        profile = generate_profile(staged_patch, staged_profile)
        if profile["patch"] != os.path.basename(output_path):
            raise ValueError("generated profile has the wrong patch identity")

        report = {
            "schema": 1,
            "operation": "translate_inverse_bind",
            "source": {
                "file": os.path.basename(source_path),
                "bytes": len(source),
                "sha256": sha256(source),
            },
            "output": {
                "file": os.path.basename(output_path),
                "bytes": len(output),
                "sha256": sha256(output),
                "changed_bytes": len(changed),
            },
            "unit_id": f"{unit_id:016x}",
            "slot": slot,
            "world_translation": list(translation),
            "mutations": mutations,
            "skipped_lods_without_slot": skipped_lods,
            "changes_only_target_translation_rows": True,
            "companions_byte_identical": True,
            "profile": profile,
        }

        publications = {
            output_path: read_file(staged_patch),
            output_path + ".gpu_resources": read_file(staged_patch + ".gpu_resources"),
            output_path + ".stream": read_file(staged_patch + ".stream"),
            profile_path: read_file(staged_profile),
            profile_path + ".json": read_file(staged_profile + ".json"),
            report_path: (json.dumps(report, indent=2) + "\n").encode("utf-8"),
        }
        for path, data in publications.items():
            write_atomic(path, data)
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    inspect_command = commands.add_parser("inspect", help="list numeric unit/LOD/slot information")
    inspect_command.add_argument("--patch", required=True)
    inspect_command.add_argument("--out", help="optional JSON report")

    profile_command = commands.add_parser("profile", help="generate a runtime profile")
    profile_command.add_argument("--patch", required=True)
    profile_command.add_argument("--out", required=True)
    profile_command.add_argument("--manifest")
    profile_command.add_argument("--unit", action="append", default=[], type=parse_unit_id)

    edit_command = commands.add_parser("translate", help="copy a patch, translate one IB slot, and profile it")
    edit_command.add_argument("--patch", required=True, help="source main patch")
    edit_command.add_argument("--out-patch", required=True,
                              help="generated main patch in another directory")
    edit_command.add_argument("--unit", required=True, type=parse_unit_id,
                              help="16-digit unit ID")
    edit_command.add_argument("--slot", required=True, type=int)
    edit_command.add_argument("--translate", required=True, type=float, nargs=3,
                              metavar=("X", "Y", "Z"))
    edit_command.add_argument("--lod", action="append", type=int,
                              help="edit one LOD; repeatable; default is every LOD containing the slot")
    edit_command.add_argument("--profile-out")
    edit_command.add_argument("--report")
    edit_command.add_argument("--force", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "inspect":
            result = inspect_patch(args.patch)
            data = (json.dumps(result, indent=2) + "\n").encode("utf-8")
            if args.out:
                write_atomic(args.out, data)
            sys.stdout.buffer.write(data)
        elif args.command == "profile":
            units = set(args.unit) if args.unit else None
            result = generate_profile(args.patch, args.out, units, args.manifest)
            print(json.dumps(result["profile"], indent=2))
        else:
            profile_path = args.profile_out or args.out_patch + ".hd2profile"
            report_path = args.report or args.out_patch + ".edit.json"
            result = edit_patch(args.patch, args.out_patch, profile_path, report_path,
                                args.unit, args.slot, tuple(args.translate),
                                args.lod, args.force)
            print(json.dumps({
                "patch": os.path.abspath(args.out_patch),
                "profile": os.path.abspath(profile_path),
                "report": os.path.abspath(report_path),
                "changed_bytes": result["output"]["changed_bytes"],
                "lods_changed": [item["lod"] for item in result["mutations"]],
            }, indent=2))
        return 0
    except (OSError, ValueError, struct.error) as error:
        print("error:", error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
