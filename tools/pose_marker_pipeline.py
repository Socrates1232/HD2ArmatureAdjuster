#!/usr/bin/env python
"""Prepare patch copies with arm control slots and runtime-locatable markers."""

from __future__ import annotations

import hashlib
import os
import shutil
import struct

from build_ib_profile import file64_to_t48, translate_world_file64
from extract_runtime_profile import UNIT_TYPE, fnv1a, write_atomic


PATCH_NAME = __import__("re").compile(r"^[0-9a-fA-F]{16}\.patch_[0-9]+$")
SECTION_FIELDS = (0x30, 0x34, 0x38, 0x3C, 0x40, 0x4C, 0x50,
                  0x54, 0x58, 0x5C, 0x60, 0x64, 0x70)
MAX_PALETTE = 256


def bundle_entries(bundle: bytes) -> list[dict]:
    _, type_count, file_count, _ = struct.unpack_from("<4I", bundle, 0)
    first = 72 + type_count * 32
    entries = []
    for index in range(file_count):
        at = first + index * 80
        file_id, type_id, data_offset = struct.unpack_from("<3Q", bundle, at)
        data_size = struct.unpack_from("<I", bundle, at + 0x38)[0]
        if data_offset + data_size > len(bundle):
            raise ValueError(f"resource {file_id:016x} is truncated")
        entries.append({"file_id": file_id, "type_id": type_id,
                        "data_offset": data_offset, "data_size": data_size,
                        "toc_offset": at})
    return entries


def read_scene(unit: bytes) -> tuple[list[tuple[int, int]], list[int]]:
    offset = struct.unpack_from("<I", unit, 0x34)[0]
    count = struct.unpack_from("<I", unit, offset)[0]
    parents_at = offset + 16 + count * 128
    hashes_at = parents_at + count * 4
    if count == 0 or hashes_at + count * 4 > len(unit):
        raise ValueError("scene graph is truncated")
    parents = [struct.unpack_from("<HH", unit, parents_at + index * 4)
               for index in range(count)]
    hashes = list(struct.unpack_from(f"<{count}I", unit, hashes_at))
    return parents, hashes


def bone_hash(name: str) -> int:
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


def branch_side(node: int, parents: list[tuple[int, int]],
                left: int | None, right: int | None) -> str:
    return branch_membership(node, parents, left, right)[0]


def branch_membership(node: int, parents: list[tuple[int, int]],
                      left: int | None, right: int | None) -> tuple[str, int | None]:
    seen = set()
    depth = 0
    while 0 <= node < len(parents) and node not in seen:
        if node == left:
            return "left", depth
        if node == right:
            return "right", depth
        seen.add(node)
        has_parent, parent = parents[node]
        if not has_parent or parent == node:
            break
        node = parent
        depth += 1
    return "other", None


def read_bone_info(unit: bytes) -> dict:
    offset = struct.unpack_from("<I", unit, 0x58)[0]
    lod_count = struct.unpack_from("<I", unit, offset)[0]
    rels = list(struct.unpack_from(f"<{lod_count}I", unit, offset + 4))
    lods = []
    data_end = 4 + lod_count * 4
    for lod, relative in enumerate(rels):
        block = offset + relative
        count, matrix_offset, real_offset, fake_offset = struct.unpack_from("<4I", unit, block)
        matrix_at = block + matrix_offset
        real_at = block + real_offset
        fake_at = block + fake_offset
        if real_at + count * 4 > len(unit) or fake_at + 4 > len(unit):
            raise ValueError(f"LOD {lod} BoneInfo is truncated")
        inverse_binds = unit[matrix_at:matrix_at + count * 64]
        real = list(struct.unpack_from(f"<{count}I", unit, real_at))
        remap_count = struct.unpack_from("<I", unit, fake_at)[0]
        remaps = []
        fake_end = 4 + remap_count * 8
        for index in range(remap_count):
            array_offset, array_count = struct.unpack_from("<2I", unit, fake_at + 4 + index * 8)
            array_at = fake_at + array_offset
            if array_at + array_count * 4 > len(unit):
                raise ValueError(f"LOD {lod} remap {index} is truncated")
            remaps.append(list(struct.unpack_from(f"<{array_count}I", unit, array_at)))
            fake_end = max(fake_end, array_offset + array_count * 4)
        block_end = fake_offset + fake_end
        data_end = max(data_end, relative + block_end)
        lods.append({"lod": lod, "count": count, "inverse_binds": inverse_binds,
                     "real": real, "remaps": remaps})
    return {"offset": offset, "lods": lods, "data_end": data_end}


def serialize_lod(inverse_binds: bytes, real: list[int], remaps: list[list[int]]) -> bytes:
    count = len(real)
    matrix_offset = 16
    real_offset = matrix_offset + len(inverse_binds)
    fake_offset = real_offset + count * 4
    fake = bytearray(struct.pack("<I", len(remaps)))
    arrays_at = 4 + len(remaps) * 8
    arrays = bytearray()
    for remap in remaps:
        fake += struct.pack("<2I", arrays_at + len(arrays), len(remap))
        arrays += struct.pack(f"<{len(remap)}I", *remap) if remap else b""
    return (struct.pack("<4I", count, matrix_offset, real_offset, fake_offset) +
            inverse_binds +
            (struct.pack(f"<{count}I", *real) if real else b"") + fake + arrays)


def serialize_bone_info(lods: list[dict]) -> bytes:
    blocks = [serialize_lod(item["inverse_binds"], item["real"], item["remaps"])
              for item in lods]
    relative = 4 + len(blocks) * 4
    output = bytearray(struct.pack("<I", len(blocks)))
    for block in blocks:
        output += struct.pack("<I", relative)
        relative += len(block)
    for block in blocks:
        output += block
    return bytes(output)


def grow_unit(unit: bytes, tail_repeats: int) -> tuple[bytes, list[dict]]:
    parents, hashes = read_scene(unit)
    left_hits = [i for i, value in enumerate(hashes) if value == bone_hash("l_shoulder")]
    right_hits = [i for i, value in enumerate(hashes) if value == bone_hash("r_shoulder")]
    left = left_hits[0] if len(left_hits) == 1 else None
    right = right_hits[0] if len(right_hits) == 1 else None
    bone = read_bone_info(unit)
    original = serialize_bone_info(bone["lods"])
    if unit[bone["offset"]:bone["offset"] + bone["data_end"]] != original:
        raise ValueError("BoneInfo does not round-trip byte-identically")

    marker_lods = []
    grown = []
    for item in bone["lods"]:
        controls = []
        for source_slot, node in enumerate(item["real"]):
            if source_slot == 0:
                continue  # HD2's uploaded palette starts at table slot 1.
            side, depth = branch_membership(node, parents, left, right)
            if side in ("left", "right"):
                controls.append({"source_slot": source_slot,
                                 "control_slot": item["count"] + len(controls),
                                 "node": node, "side": side, "depth": depth})
        added = len(controls) + 1 + tail_repeats
        if item["count"] + added > MAX_PALETTE:
            raise ValueError(f"LOD {item['lod']} palette exceeds {MAX_PALETTE} entries")
        donor = next((slot for slot, node in enumerate(item["real"])
                      if branch_side(node, parents, left, right) == "other"), 0)
        donor_matrix = item["inverse_binds"][donor * 64:(donor + 1) * 64]
        donor_node = item["real"][donor]
        probe_matrix, _ = translate_world_file64(
            donor_matrix, (added * 0.0001, 0.0, 0.0))
        control_matrices = b"".join(
            item["inverse_binds"][control["source_slot"] * 64:
                                  (control["source_slot"] + 1) * 64]
            for control in controls)
        control_nodes = [control["node"] for control in controls]
        control_for = {control["source_slot"]: control["control_slot"]
                       for control in controls}
        remaps = [[control_for.get(slot, slot) for slot in remap]
                  for remap in item["remaps"]]
        probe_slot = item["count"] + len(controls)
        grown.append({"lod": item["lod"], "count": item["count"] + added,
                      "inverse_binds": item["inverse_binds"] + control_matrices +
                                       probe_matrix + donor_matrix * tail_repeats,
                      "real": item["real"] + control_nodes +
                              [donor_node] * (tail_repeats + 1),
                      "remaps": remaps})
        marker_lods.append({"lod": item["lod"],
                            "first_control_slot": item["count"],
                            "probe_slot": probe_slot,
                            "entries": item["count"] + added,
                            "donor_slot": donor, "donor_node": donor_node,
                            "controls": controls})

    new_section = serialize_bone_info(grown)
    section_offsets = [struct.unpack_from("<I", unit, field)[0] for field in SECTION_FIELDS]
    next_section = min(value for value in section_offsets if value > bone["offset"])
    old_end = bone["offset"] + bone["data_end"]
    if any(unit[old_end:next_section]):
        raise ValueError("BoneInfo alignment padding is not zero")
    new_end = bone["offset"] + len(new_section)
    new_pad = (-new_end) % 16
    replacement = new_section + bytes(new_pad)
    delta = len(replacement) - (next_section - bone["offset"])
    if delta <= 0 or delta % 16:
        raise ValueError("palette growth did not preserve section alignment")
    output = bytearray(unit[:bone["offset"]] + replacement + unit[next_section:])
    for field in SECTION_FIELDS:
        value = struct.unpack_from("<I", output, field)[0]
        if value > bone["offset"]:
            struct.pack_into("<I", output, field, value + delta)
    ending = struct.unpack_from("<I", output, 0x60)[0]
    if len(output) != ending + 8:
        raise ValueError("grown unit no longer ends at EndingOffset + 8")
    return bytes(output), marker_lods


def replace_bundle_unit(bundle: bytes, file_id: int, grown: bytes) -> bytes:
    entries = bundle_entries(bundle)
    matches = [entry for entry in entries
               if entry["file_id"] == file_id and entry["type_id"] == UNIT_TYPE]
    if len(matches) != 1:
        raise ValueError(f"expected one unit {file_id:016x}, found {len(matches)}")
    target = matches[0]
    delta = len(grown) - target["data_size"]
    output = bytearray(bundle[:target["data_offset"]] + grown +
                       bundle[target["data_offset"] + target["data_size"]:])
    for entry in entries:
        if entry["data_offset"] > target["data_offset"]:
            struct.pack_into("<Q", output, entry["toc_offset"] + 0x10,
                             entry["data_offset"] + delta)
    struct.pack_into("<I", output, target["toc_offset"] + 0x38, len(grown))
    return bytes(output)


def shoulder_units(bundle: bytes) -> list[dict]:
    result = []
    wanted = {bone_hash("l_shoulder"), bone_hash("r_shoulder")}
    for entry in bundle_entries(bundle):
        if entry["type_id"] != UNIT_TYPE:
            continue
        unit = bundle[entry["data_offset"]:entry["data_offset"] + entry["data_size"]]
        try:
            _, hashes = read_scene(unit)
            bone = read_bone_info(unit)
        except (ValueError, struct.error):
            continue
        if not wanted.intersection(hashes):
            continue
        signature = hashlib.sha256(struct.pack("<Q", entry["file_id"]) +
                                   serialize_bone_info(bone["lods"])).hexdigest()
        result.append({"unit_id": entry["file_id"], "signature": signature})
    return result


def prepare_pose_tree(root: str, output_root: str, marker_min: int) -> dict:
    root = os.path.abspath(root)
    output_root = os.path.abspath(output_root)
    try:
        common = os.path.commonpath((root, output_root))
    except ValueError:
        common = None
    if common == root:
        raise ValueError("--out-root must not be inside the source tree")
    if "steamapps" in output_root.lower() or "helldivers 2" in output_root.lower():
        raise ValueError("refusing to prepare markers directly in the game directory")
    if os.path.exists(output_root):
        raise ValueError("--out-root already exists")

    patches = []
    variants = {}
    for directory, child_directories, files in os.walk(root):
        child_directories[:] = [name for name in child_directories
                                if name != "HD2ArmatureProfiles"]
        for name in files:
            if not PATCH_NAME.fullmatch(name):
                continue
            path = os.path.join(directory, name)
            bundle = open(path, "rb").read()
            units = shoulder_units(bundle)
            if units:
                relative = os.path.relpath(path, root)
                patches.append({"path": path, "relative": relative, "units": units})
                for unit in units:
                    variants.setdefault(unit["signature"], unit["unit_id"])

    ordered_variants = sorted(variants.items(), key=lambda item: (item[1], item[0]))
    repeats = {signature: marker_min + index
               for index, (signature, _) in enumerate(ordered_variants)}
    if repeats and max(repeats.values()) + 1 >= MAX_PALETTE:
        raise ValueError("too many marker variants for the palette index range")

    shutil.copytree(root, output_root,
                    ignore=shutil.ignore_patterns("HD2ArmatureProfiles"))
    marker_records = {}
    patch_reports = []
    for patch in patches:
        output_path = os.path.join(output_root, patch["relative"])
        bundle = open(output_path, "rb").read()
        marked_units = []
        for unit_plan in patch["units"]:
            current = next(entry for entry in bundle_entries(bundle)
                           if entry["file_id"] == unit_plan["unit_id"] and
                           entry["type_id"] == UNIT_TYPE)
            unit = bundle[current["data_offset"]:current["data_offset"] + current["data_size"]]
            tail_repeats = repeats[unit_plan["signature"]]
            grown, lods = grow_unit(unit, tail_repeats)
            bundle = replace_bundle_unit(bundle, unit_plan["unit_id"], grown)
            marked_units.append({"unit_id": f"{unit_plan['unit_id']:016x}",
                                 "variant": unit_plan["signature"],
                                 "tail_repeats": tail_repeats, "lods": lods})
        write_atomic(output_path, bundle)

        for marked in marked_units:
            current = next(entry for entry in bundle_entries(bundle)
                           if entry["file_id"] == int(marked["unit_id"], 16) and
                           entry["type_id"] == UNIT_TYPE)
            unit = bundle[current["data_offset"]:current["data_offset"] + current["data_size"]]
            bone = read_bone_info(unit)
            for lod, marker_lod in zip(bone["lods"], marked["lods"]):
                table = file64_to_t48(lod["inverse_binds"])
                key = (int(marked["unit_id"], 16), fnv1a(table), lod["count"])
                record = marker_records.setdefault(key, {
                    "unit_id": marked["unit_id"], "table_key": f"{key[1]:016x}",
                    "entries": lod["count"],
                    "first_control_slot": marker_lod["first_control_slot"],
                    "probe_slot": marker_lod["probe_slot"],
                    "tail_repeats": marked["tail_repeats"],
                    "controls": marker_lod["controls"], "sources": []})
                expected = (marker_lod["first_control_slot"], marker_lod["probe_slot"],
                            marked["tail_repeats"], marker_lod["controls"])
                if (record["first_control_slot"], record["probe_slot"], record["tail_repeats"],
                        record["controls"]) != expected:
                    raise ValueError("one runtime table received conflicting marker layouts")
                record["sources"].append({"patch": patch["relative"], "lod": lod["lod"]})
        patch_reports.append({"patch": patch["relative"], "units": marked_units})

    return {"schema": 1, "operation": "prepare_pose_marker_tree",
            "source_root": root, "output_root": output_root,
            "marker_min": marker_min, "variants": len(variants),
            "patches_marked": len(patches),
            "markers": sorted(marker_records.values(),
                              key=lambda item: (item["tail_repeats"], item["entries"])),
            "patches": patch_reports}
