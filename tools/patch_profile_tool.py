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

from build_ib_profile import (file64_to_t48, inverse_bind_tables, read_file, sha256,
                              translate_world_file64)
from extract_runtime_profile import (HEADER_SIZE, MAGIC, RECORD_HEADER_SIZE,
                                     UNIT_TYPE, bundle_entries, fnv1a, generate_profile,
                                     write_atomic)


PATCH_NAME = re.compile(r"^[0-9a-fA-F]{16}\.patch_[0-9]+$")
UNIT_ID = re.compile(r"^[0-9a-fA-F]{16}$")
MASK64 = 0xFFFFFFFFFFFFFFFF


def parse_unit_id(value: str) -> int:
    if not UNIT_ID.fullmatch(value):
        raise argparse.ArgumentTypeError("unit ID must contain exactly 16 hex digits")
    return int(value, 16)


def unit_entries(bundle: bytes) -> list[dict]:
    return [entry for entry in bundle_entries(bundle)
            if entry["type_id"] == UNIT_TYPE]


def bone_hash(name: str) -> int:
    data = name.encode("utf-8")
    multiplier = 0xC6A4A7935BD1E995
    value = (len(data) * multiplier) & MASK64
    whole = len(data) // 8
    for index in range(whole):
        item = struct.unpack_from("<Q", data, index * 8)[0]
        item = (item * multiplier) & MASK64
        item ^= item >> 47
        item = (item * multiplier) & MASK64
        value ^= item
        value = (value * multiplier) & MASK64
    tail = data[whole * 8:]
    for index in range(len(tail) - 1, -1, -1):
        value ^= tail[index] << (8 * index)
    if tail:
        value = (value * multiplier) & MASK64
    value ^= value >> 47
    value = (value * multiplier) & MASK64
    value ^= value >> 47
    return value >> 32


def armature_semantics(unit: bytes) -> tuple[list[tuple[int, int]], list[int], list[dict]]:
    if len(unit) < 0x5C:
        raise ValueError("unit header is truncated")
    scene_offset = struct.unpack_from("<I", unit, 0x34)[0]
    bone_offset = struct.unpack_from("<I", unit, 0x58)[0]
    if scene_offset == 0 or scene_offset + 16 > len(unit):
        raise ValueError("TransformInfo offset is absent or outside the unit")
    node_count = struct.unpack_from("<I", unit, scene_offset)[0]
    if node_count == 0 or node_count > 65535:
        raise ValueError("scene-graph node count is invalid")
    parent_offset = scene_offset + 16 + node_count * 128
    hash_offset = parent_offset + node_count * 4
    if hash_offset + node_count * 4 > len(unit):
        raise ValueError("scene graph is truncated")
    parents = [struct.unpack_from("<HH", unit, parent_offset + index * 4)
               for index in range(node_count)]
    hashes = [struct.unpack_from("<I", unit, hash_offset + index * 4)[0]
              for index in range(node_count)]

    if bone_offset == 0 or bone_offset + 4 > len(unit):
        raise ValueError("BoneInfo offset is absent or outside the unit")
    lod_count = struct.unpack_from("<I", unit, bone_offset)[0]
    if lod_count == 0 or bone_offset + 4 + lod_count * 4 > len(unit):
        raise ValueError("BoneInfo LOD list is invalid")
    palettes = []
    for lod in range(lod_count):
        relative = struct.unpack_from("<I", unit, bone_offset + 4 + lod * 4)[0]
        block = bone_offset + relative
        if block + 16 > len(unit):
            raise ValueError(f"LOD {lod} BoneInfo block is truncated")
        entries, _, real_offset, _ = struct.unpack_from("<4I", unit, block)
        start = block + real_offset
        if start + entries * 4 > len(unit):
            raise ValueError(f"LOD {lod} RealIndices table is truncated")
        palettes.append({
            "lod": lod,
            "real": list(struct.unpack_from(f"<{entries}I", unit, start)) if entries else [],
        })
    return parents, hashes, palettes


def branch_membership(node: int, parents: list[tuple[int, int]],
                      left_root: int | None,
                      right_root: int | None) -> tuple[str, int | None]:
    seen = set()
    current = node
    depth = 0
    while 0 <= current < len(parents) and current not in seen:
        if current == left_root:
            return "left", depth
        if current == right_root:
            return "right", depth
        seen.add(current)
        has_parent, parent = parents[current]
        if not has_parent or parent == current:
            break
        current = parent
        depth += 1
    return "other", None


def active_runtime_tables(profile_directory: str | None) -> set[tuple[int, int, int]] | None:
    if profile_directory is None:
        return None
    active_path = os.path.join(profile_directory, "active_profiles.txt")
    if not os.path.isfile(active_path):
        raise ValueError("profile directory has no active_profiles.txt")
    active = set()
    with open(active_path, encoding="utf-8") as stream:
        names = [line.strip() for line in stream if line.strip()]
    if not names:
        raise ValueError("active_profiles.txt contains no profiles")
    for name in names:
        if os.path.basename(name) != name or not name.endswith(".hd2profile"):
            raise ValueError("active_profiles.txt contains an invalid filename")
        for unit_id, entries, table in profile_runtime_keys(
                os.path.join(profile_directory, name)):
            active.add((unit_id, fnv1a(table), entries))
    return active


def generate_branch_targets(root: str, output_path: str, report_path: str,
                            left_translation: tuple[float, float, float],
                            right_translation: tuple[float, float, float],
                            profile_directory: str | None, falloff_depth: int,
                            force: bool) -> dict:
    root = os.path.abspath(root)
    output_path = os.path.abspath(output_path)
    report_path = os.path.abspath(report_path)
    if not os.path.isdir(root):
        raise ValueError("--root is not a directory")
    refuse_game_output(output_path)
    require_new_outputs([output_path, report_path], force)
    active = active_runtime_tables(profile_directory)
    patches = []
    for directory, child_directories, files in os.walk(root):
        child_directories.sort(key=str.casefold)
        files.sort(key=str.casefold)
        if profile_directory and os.path.normcase(os.path.abspath(directory)) == os.path.normcase(
                os.path.abspath(profile_directory)):
            child_directories[:] = []
            continue
        for name in files:
            if PATCH_NAME.fullmatch(name):
                patches.append(os.path.join(directory, name))
    patches.sort(key=lambda path: os.path.relpath(path, root).replace("\\", "/").casefold())
    if not patches:
        raise ValueError("no patch main files were found under --root")

    left_hash, right_hash = bone_hash("l_shoulder"), bone_hash("r_shoulder")
    observations: dict[tuple[int, int, int], dict] = {}
    table_bytes: dict[tuple[int, int, int], bytes] = {}
    skipped = []
    tables_seen = set()
    for patch in patches:
        relative = os.path.relpath(patch, root).replace("\\", "/")
        bundle = read_file(patch)
        for entry in unit_entries(bundle):
            unit_id = entry["file_id"]
            unit = bundle[entry["data_offset"]:entry["data_offset"] + entry["data_size"]]
            try:
                parents, hashes, palettes = armature_semantics(unit)
                tables = inverse_bind_tables(unit)
            except (ValueError, struct.error) as error:
                skipped.append({"patch": relative, "unit_id": f"{unit_id:016x}",
                                "reason": str(error)})
                continue
            left_nodes = [index for index, value in enumerate(hashes) if value == left_hash]
            right_nodes = [index for index, value in enumerate(hashes) if value == right_hash]
            if len(left_nodes) > 1 or len(right_nodes) > 1:
                raise ValueError(f"{relative}: unit {unit_id:016x} has duplicate shoulder nodes")
            if not left_nodes and not right_nodes:
                continue
            if len(tables) != len(palettes):
                raise ValueError(f"{relative}: unit {unit_id:016x} has mismatched LOD metadata")
            left_root = left_nodes[0] if left_nodes else None
            right_root = right_nodes[0] if right_nodes else None
            for table, palette in zip(tables, palettes):
                t48 = file64_to_t48(table["file64"])
                table_key = fnv1a(t48)
                runtime_key = (unit_id, table_key, table["bones"])
                if active is not None and runtime_key not in active:
                    continue
                previous = table_bytes.setdefault(runtime_key, t48)
                if previous != t48:
                    raise ValueError(f"FNV collision for unit {unit_id:016x} table {table_key:016x}")
                tables_seen.add(runtime_key)
                if len(palette["real"]) != table["bones"]:
                    raise ValueError(f"{relative}: unit {unit_id:016x} LOD {table['lod']} palette mismatch")
                for slot, node in enumerate(palette["real"]):
                    side, depth = branch_membership(node, parents, left_root, right_root)
                    key = (unit_id, table_key, slot)
                    item = observations.setdefault(key, {
                        "sides": set(), "depths": set(), "sources": [],
                        "entries": table["bones"]})
                    item["sides"].add(side)
                    if depth is not None:
                        item["depths"].add(depth)
                    item["sources"].append({"patch": relative, "lod": table["lod"],
                                            "node": node, "side": side,
                                            "depth": depth})

    if active is not None:
        missing = active - tables_seen
        # Missing tables without shoulder roots are expected; only mappings observed above matter.
        active_tables_considered = len(active) - len(missing)
    else:
        active_tables_considered = len(tables_seen)
    targets = []
    excluded = []
    conflicts = []
    for (unit_id, table_key, slot), item in sorted(observations.items()):
        branch = item["sides"] & {"left", "right"}
        if not branch:
            continue
        if len(item["sides"]) != 1 or len(item["depths"]) != 1:
            conflicts.append({
                "unit_id": f"{unit_id:016x}", "table_key": f"{table_key:016x}",
                "slot": slot, "sides": sorted(item["sides"]),
                "depths": sorted(item["depths"]), "sources": item["sources"],
            })
            continue
        side = next(iter(branch))
        depth = next(iter(item["depths"]))
        if falloff_depth >= 0 and depth >= falloff_depth:
            excluded.append({
                "unit_id": f"{unit_id:016x}", "table_key": f"{table_key:016x}",
                "slot": slot, "side": side, "depth": depth,
                "reason": "translation reaches zero at the configured falloff depth",
                "sources": item["sources"],
            })
            continue
        scale = 1.0 if falloff_depth < 0 else (falloff_depth - depth) / falloff_depth
        base_translation = left_translation if side == "left" else right_translation
        translation = [value * scale for value in base_translation]
        targets.append({
            "unit_id": f"{unit_id:016x}", "table_key": f"{table_key:016x}",
            "slot": slot, "side": side, "depth": depth, "scale": scale,
            "translation": translation,
            "sources": item["sources"],
        })
    if conflicts:
        raise ValueError("a runtime table reuses a target slot with conflicting branch semantics; "
                         "see generated data before changing the profile format")
    if not targets:
        raise ValueError("no shoulder or descendant palette slots were found")

    lines = [
        "# unit_id table_fingerprint slot translate_x translate_y translate_z",
        "# Generated from scene-graph parent links and per-LOD RealIndices.",
    ]
    for target in targets:
        values = " ".join(f"{value:+.9g}" for value in target["translation"])
        lines.append(f"{target['unit_id']} {target['table_key']} {target['slot']} {values}")
    result = {
        "schema": 1,
        "operation": "generate_table_qualified_shoulder_branches",
        "root": root,
        "profile_directory": os.path.abspath(profile_directory) if profile_directory else None,
        "patches_scanned": len(patches),
        "active_tables_considered": active_tables_considered,
        "armature_tables_observed": len(tables_seen),
        "falloff_depth": falloff_depth,
        "targets": targets,
        "excluded_targets": excluded,
        "targets_by_side": {
            "left": sum(item["side"] == "left" for item in targets),
            "right": sum(item["side"] == "right" for item in targets),
        },
        "skipped_units": skipped,
        "conflicts": conflicts,
    }
    write_atomic(output_path, ("\n".join(lines) + "\n").encode("utf-8"))
    write_atomic(report_path, (json.dumps(result, indent=2) + "\n").encode("utf-8"))
    return result


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


def profile_runtime_keys(path: str) -> list[tuple[int, int, bytes]]:
    data = read_file(path)
    if len(data) < HEADER_SIZE or data[:len(MAGIC)] != MAGIC:
        raise ValueError("generated profile header is invalid")
    record_count = struct.unpack_from("<I", data, 12)[0]
    name_size = struct.unpack_from("<I", data, 16)[0]
    cursor = HEADER_SIZE + name_size
    keys = []
    for _ in range(record_count):
        if cursor + RECORD_HEADER_SIZE > len(data):
            raise ValueError("generated profile record is truncated")
        unit_id = struct.unpack_from("<Q", data, cursor)[0]
        entries = struct.unpack_from("<I", data, cursor + 12)[0]
        table_size = struct.unpack_from("<I", data, cursor + 20)[0]
        cursor += RECORD_HEADER_SIZE
        if cursor + table_size > len(data):
            raise ValueError("generated profile table is truncated")
        keys.append((unit_id, entries, data[cursor:cursor + table_size]))
        cursor += table_size
    if cursor != len(data):
        raise ValueError("generated profile has trailing data")
    return keys


def profile_tree(root: str, output_directory: str, force: bool) -> dict:
    root = os.path.abspath(root)
    output_directory = os.path.abspath(output_directory)
    if not os.path.isdir(root):
        raise ValueError("--root is not a directory")

    patches = []
    for directory, child_directories, files in os.walk(root):
        child_directories.sort(key=str.casefold)
        files.sort(key=str.casefold)
        if os.path.normcase(os.path.abspath(directory)) == os.path.normcase(output_directory):
            child_directories[:] = []
            continue
        for name in files:
            if PATCH_NAME.fullmatch(name):
                patches.append(os.path.join(directory, name))
    patches.sort(key=lambda path: os.path.relpath(path, root).replace("\\", "/").casefold())
    if not patches:
        raise ValueError("no patch main files were found under --root")

    outputs = []
    for patch in patches:
        relative = os.path.relpath(patch, root).replace("\\", "/")
        prefix = sha256(relative.casefold().encode("utf-8"))[:16]
        profile_name = prefix + "__" + os.path.basename(patch) + ".hd2profile"
        outputs.extend((os.path.join(output_directory, profile_name),
                        os.path.join(output_directory, profile_name + ".json")))
    active_path = os.path.join(output_directory, "active_profiles.txt")
    manifest_path = os.path.join(output_directory, "profile_tree_manifest.json")
    require_new_outputs(outputs + [active_path, manifest_path], force)
    os.makedirs(output_directory, exist_ok=True)

    covered = set()
    active_profiles = []
    generated = []
    skipped = []
    source_records = 0
    for index, patch in enumerate(patches):
        relative = os.path.relpath(patch, root).replace("\\", "/")
        profile_path = outputs[index * 2]
        try:
            profile = generate_profile(patch, profile_path)
        except (ValueError, struct.error) as error:
            skipped.append({"patch": relative, "reason": str(error)})
            continue
        keys = profile_runtime_keys(profile_path)
        new_keys = [key for key in keys if key not in covered]
        active = bool(new_keys)
        if active:
            active_profiles.append(os.path.basename(profile_path))
            covered.update(new_keys)
        source_records += len(keys)
        generated.append({
            "patch": relative,
            "patch_sha256": profile["triplet"]["main"]["sha256"],
            "profile": os.path.basename(profile_path),
            "records": len(keys),
            "new_runtime_tables": len(new_keys),
            "active": active,
        })

    unit_variants = {}
    for unit_id, entries, table in covered:
        unit_variants.setdefault(f"{unit_id:016x}", []).append({
            "entries": entries,
            "t48_sha256": sha256(table),
        })
    for variants in unit_variants.values():
        variants.sort(key=lambda item: (item["entries"], item["t48_sha256"]))

    result = {
        "schema": 1,
        "root": root,
        "patches_discovered": len(patches),
        "profiles_generated": len(generated),
        "profiles_active": len(active_profiles),
        "source_records": source_records,
        "unique_runtime_tables": len(covered),
        "duplicate_source_records": source_records - len(covered),
        "distinct_units": len(unit_variants),
        "unit_variants": unit_variants,
        "profiles": generated,
        "skipped_patches": skipped,
    }
    write_atomic(active_path, ("\n".join(active_profiles) + "\n").encode("utf-8"))
    write_atomic(manifest_path, (json.dumps(result, indent=2) + "\n").encode("utf-8"))
    return result


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

    tree_command = commands.add_parser(
        "profile-tree", help="profile every patch under a directory and build an active union")
    tree_command.add_argument("--root", required=True)
    tree_command.add_argument("--out-dir", required=True)
    tree_command.add_argument("--force", action="store_true")

    branch_command = commands.add_parser(
        "shoulder-targets",
        help="derive table-qualified shoulder and descendant slots from scene graphs")
    branch_command.add_argument("--root", required=True, help="profiled mod-copy root")
    branch_command.add_argument("--profile-dir", required=True,
                                help="directory containing active_profiles.txt")
    branch_command.add_argument("--out", help="default: <profile-dir>/shoulder_targets.txt")
    branch_command.add_argument("--report",
                                help="default: <profile-dir>/shoulder_targets.json")
    branch_command.add_argument("--left-translate", type=float, nargs=3,
                                default=(0.03, 0.0, 0.0), metavar=("X", "Y", "Z"))
    branch_command.add_argument("--right-translate", type=float, nargs=3,
                                default=(-0.03, 0.0, 0.0), metavar=("X", "Y", "Z"))
    branch_command.add_argument(
        "--falloff-depth", type=int, default=3,
        help="translation reaches zero at this descendant depth (default: 3); "
             "use -1 for the old uniform full-branch diagnostic")
    branch_command.add_argument("--force", action="store_true")

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
        elif args.command == "profile-tree":
            result = profile_tree(args.root, args.out_dir, args.force)
            print(json.dumps({
                "output_directory": os.path.abspath(args.out_dir),
                "patches_discovered": result["patches_discovered"],
                "profiles_generated": result["profiles_generated"],
                "profiles_active": result["profiles_active"],
                "unique_runtime_tables": result["unique_runtime_tables"],
                "distinct_units": result["distinct_units"],
                "skipped_patches": len(result["skipped_patches"]),
            }, indent=2))
        elif args.command == "shoulder-targets":
            left = tuple(args.left_translate)
            right = tuple(args.right_translate)
            if (not all(math.isfinite(value) and abs(value) <= 10.0
                        for value in left + right) or
                    not any(value != 0.0 for value in left) or
                    not any(value != 0.0 for value in right)):
                raise ValueError("branch translations must be finite, nonzero, and within +/-10 metres")
            if args.falloff_depth == 0 or args.falloff_depth < -1:
                raise ValueError("--falloff-depth must be -1 or a positive integer")
            output = args.out or os.path.join(args.profile_dir, "shoulder_targets.txt")
            report = args.report or os.path.join(args.profile_dir, "shoulder_targets.json")
            result = generate_branch_targets(args.root, output, report, left, right,
                                             args.profile_dir, args.falloff_depth, args.force)
            print(json.dumps({
                "output": os.path.abspath(output),
                "report": os.path.abspath(report),
                "patches_scanned": result["patches_scanned"],
                "armature_tables_observed": result["armature_tables_observed"],
                "targets": len(result["targets"]),
                "targets_by_side": result["targets_by_side"],
            }, indent=2))
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
