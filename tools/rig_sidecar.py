#!/usr/bin/env python
"""Export versioned semantic rig sidecars without modifying patch data."""

from __future__ import annotations

import json
import math
import os
import re
import struct

from build_ib_profile import (file64_to_t48, inverse_bind_tables, read_file, sha256,
                              triplet_hashes)
from extract_runtime_profile import UNIT_TYPE, bundle_entries, fnv1a, write_atomic


PATCH_NAME = re.compile(r"^[0-9a-fA-F]{16}\.patch_[0-9]+$")
ROLE_KEYS = ("chest", "left_shoulder", "right_shoulder", "left_elbow",
             "right_elbow", "left_hand", "right_hand")
DEFAULT_ROLES = {"left_shoulder": "l_shoulder", "right_shoulder": "r_shoulder"}
MASK64 = 0xFFFFFFFFFFFFFFFF


class NotRig(ValueError):
    """The unit has no scene graph or BoneInfo and is not a rig sidecar candidate."""


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


def parse_role_assignments(values: list[str]) -> dict[str, str]:
    roles = dict(DEFAULT_ROLES)
    for value in values:
        key, separator, name = value.partition("=")
        if not separator or key not in ROLE_KEYS or not name:
            raise ValueError("--role must be one of " + ", ".join(ROLE_KEYS) +
                             " followed by =NODE_NAME")
        roles[key] = name
    return roles


def _matrix_product(left: list[float], right: list[float]) -> list[float]:
    result = [0.0] * 16
    for row in range(4):
        for column in range(4):
            result[row * 4 + column] = sum(
                left[row * 4 + inner] * right[inner * 4 + column]
                for inner in range(4))
    return result


def _local_matrix(values: tuple[float, ...]) -> list[float]:
    rotation = values[:9]
    scale = values[12:15]
    return [rotation[0] * scale[0], rotation[1] * scale[0], rotation[2] * scale[0], 0.0,
            rotation[3] * scale[1], rotation[4] * scale[1], rotation[5] * scale[1], 0.0,
            rotation[6] * scale[2], rotation[7] * scale[2], rotation[8] * scale[2], 0.0,
            values[9], values[10], values[11], 1.0]


def _validate_parents(raw_parents: list[tuple[int, int]]) -> list[int | None]:
    count = len(raw_parents)
    parents: list[int | None] = []
    for node, (has_parent, parent) in enumerate(raw_parents):
        if has_parent not in (0, 1):
            raise ValueError(f"scene node {node} has invalid parent-present flag {has_parent}")
        if not has_parent:
            parents.append(None)
        elif parent >= count:
            raise ValueError(f"scene node {node} has out-of-range parent {parent}")
        else:
            parents.append(parent)

    for start in range(count):
        path = set()
        node: int | None = start
        while node is not None:
            if node in path:
                raise ValueError(f"scene parent cycle reaches node {node}")
            path.add(node)
            node = parents[node]
    return parents


def _read_scene(unit: bytes) -> dict:
    if len(unit) < 0x5C:
        raise NotRig("unit header is truncated")
    scene_offset = struct.unpack_from("<I", unit, 0x34)[0]
    bone_offset = struct.unpack_from("<I", unit, 0x58)[0]
    if scene_offset == 0 or bone_offset == 0:
        raise NotRig("unit has no TransformInfo/BoneInfo pair")
    if scene_offset + 16 > len(unit):
        raise ValueError("TransformInfo offset is outside the unit")
    count = struct.unpack_from("<I", unit, scene_offset)[0]
    if count == 0 or count > 65535:
        raise ValueError("scene-graph node count is invalid")
    end = scene_offset + 16 + count * 136
    if end > len(unit):
        raise ValueError("scene graph is truncated")

    local_base = scene_offset + 16
    matrix_base = local_base + count * 64
    parent_base = matrix_base + count * 64
    hash_base = parent_base + count * 4
    raw_parents = [struct.unpack_from("<HH", unit, parent_base + index * 4)
                   for index in range(count)]
    parents = _validate_parents(raw_parents)
    hashes = [struct.unpack_from("<I", unit, hash_base + index * 4)[0]
              for index in range(count)]
    local_records = [struct.unpack_from("<15f", unit, local_base + index * 64)
                     for index in range(count)]
    stored_matrices = [list(struct.unpack_from("<16f", unit, matrix_base + index * 64))
                       for index in range(count)]
    nodes = []
    worst_error = 0.0
    worst_node = None
    for index in range(count):
        local_offset = local_base + index * 64
        local_values = local_records[index]
        if not all(math.isfinite(value) for value in local_values):
            raise ValueError(f"scene node {index} has non-finite local TransformData")
        dummy = struct.unpack_from("<I", unit, local_offset + 60)[0]
        stored = stored_matrices[index]
        if not all(math.isfinite(value) for value in stored):
            raise ValueError(f"scene node {index} has a non-finite stored rest matrix")
        local_matrix = _local_matrix(local_values)
        expected = (local_matrix if parents[index] is None else
                    _matrix_product(local_matrix, stored_matrices[parents[index]]))
        error = max(abs(left - right) for left, right in zip(expected, stored))
        if error > worst_error:
            worst_error, worst_node = error, index
        nodes.append({
            "index": index,
            "name_hash": f"{hashes[index]:08x}",
            "names": [],
            "raw_parent": {"present": raw_parents[index][0], "index": raw_parents[index][1]},
            "parent": parents[index],
            "local_transform": {
                "encoding": "TransformData(rot3x3,pos3,scale3,u32)",
                "raw_hex": unit[local_offset:local_offset + 64].hex(),
                "rotation": [list(local_values[0:3]), list(local_values[3:6]),
                             list(local_values[6:9])],
                "position": list(local_values[9:12]),
                "scale": list(local_values[12:15]),
                "dummy_u32": dummy,
            },
            "stored_rest_matrix": stored,
        })
    return {
        "offset": scene_offset,
        "bytes": 16 + count * 136,
        "header_padding_hex": unit[scene_offset + 4:scene_offset + 16].hex(),
        "node_count": count,
        "nodes": nodes,
        "hashes": hashes,
        "parents": parents,
        "rest_validation": {
            "equation": "stored_world = local_transform * stored_parent_world",
            "max_absolute_error": worst_error,
            "worst_node": worst_node,
            "within_2e-5": worst_error < 2.0e-5,
        },
    }


def _resolve_roles(scene: dict, configured: dict[str, str]) -> dict:
    roles = {}
    claimed_nodes = {}
    for key in ROLE_KEYS:
        name = configured.get(key)
        if name is None:
            roles[key] = {"status": "unconfigured", "name": None,
                          "name_hash": None, "node": None}
            continue
        name_hash = bone_hash(name)
        matches = [index for index, value in enumerate(scene["hashes"])
                   if value == name_hash]
        if len(matches) > 1:
            raise ValueError(f"semantic role {key} ({name}) matches multiple scene nodes")
        if not matches:
            roles[key] = {"status": "missing", "name": name,
                          "name_hash": f"{name_hash:08x}", "node": None}
            continue
        node = matches[0]
        if node in claimed_nodes and claimed_nodes[node] != key:
            raise ValueError(f"scene node {node} is assigned to both {claimed_nodes[node]} and {key}")
        claimed_nodes[node] = key
        scene["nodes"][node]["names"].append(name)
        roles[key] = {"status": "resolved", "name": name,
                      "name_hash": f"{name_hash:08x}", "node": node}
    return roles


def _branch_nodes(scene: dict, root: int | None) -> list[dict]:
    if root is None:
        return []
    result = []
    for candidate in range(scene["node_count"]):
        node: int | None = candidate
        depth = 0
        while node is not None:
            if node == root:
                result.append({"node": candidate, "depth": depth})
                break
            node = scene["parents"][node]
            depth += 1
    return result


def _is_descendant(scene: dict, node: int, ancestor: int) -> bool:
    current: int | None = node
    while current is not None:
        if current == ancestor:
            return True
        current = scene["parents"][current]
    return False


def _build_sidecar(patch_relative: str, patch_triplet: dict, entry: dict,
                   unit: bytes, configured_roles: dict[str, str]) -> dict:
    scene = _read_scene(unit)
    roles = _resolve_roles(scene, configured_roles)
    left_root = roles["left_shoulder"]["node"]
    right_root = roles["right_shoulder"]["node"]
    left_nodes = _branch_nodes(scene, left_root)
    right_nodes = _branch_nodes(scene, right_root)
    left_lookup = {item["node"]: item["depth"] for item in left_nodes}
    right_lookup = {item["node"]: item["depth"] for item in right_nodes}
    overlap = sorted(set(left_lookup) & set(right_lookup))
    if overlap:
        raise ValueError("left/right shoulder descendant sets overlap at scene nodes " +
                         ", ".join(map(str, overlap)))
    for side, lookup in (("left", left_lookup), ("right", right_lookup)):
        for part in ("elbow", "hand"):
            role = roles[f"{side}_{part}"]
            if role["status"] == "resolved" and role["node"] not in lookup:
                raise ValueError(f"resolved {side}_{part} is outside the {side} shoulder branch")
        elbow = roles[f"{side}_elbow"]["node"]
        hand = roles[f"{side}_hand"]["node"]
        if elbow is not None and hand is not None and not _is_descendant(scene, hand, elbow):
            raise ValueError(f"resolved {side}_hand is not a descendant of {side}_elbow")
    chest = roles["chest"]["node"]
    if chest is not None:
        for side, root in (("left", left_root), ("right", right_root)):
            if root is not None and not _is_descendant(scene, root, chest):
                raise ValueError(f"resolved {side}_shoulder is not a descendant of chest")

    tables = inverse_bind_tables(unit)
    bone_offset = struct.unpack_from("<I", unit, 0x58)[0]
    lod_count = struct.unpack_from("<I", unit, bone_offset)[0]
    if len(tables) != lod_count:
        raise ValueError("BoneInfo table count disagrees with inverse-bind parser")
    lods = []
    for table in tables:
        relative = struct.unpack_from("<I", unit, bone_offset + 4 + table["lod"] * 4)[0]
        block = bone_offset + relative
        entries, _, real_offset, _ = struct.unpack_from("<4I", unit, block)
        real_start = block + real_offset
        if real_start + entries * 4 > len(unit):
            raise ValueError(f"LOD {table['lod']} RealIndices table is truncated")
        slot_to_node = list(struct.unpack_from(f"<{entries}I", unit, real_start)) if entries else []
        for slot, node in enumerate(slot_to_node):
            if node >= scene["node_count"]:
                raise ValueError(f"LOD {table['lod']} slot {slot} maps outside the scene graph")
        left_slots = [{"slot": slot, "node": node, "depth": left_lookup[node]}
                      for slot, node in enumerate(slot_to_node) if node in left_lookup]
        right_slots = [{"slot": slot, "node": node, "depth": right_lookup[node]}
                       for slot, node in enumerate(slot_to_node) if node in right_lookup]
        t48 = file64_to_t48(table["file64"])
        lods.append({
            "lod": table["lod"],
            "entry_count": entries,
            "slot_to_scene_node": slot_to_node,
            "table": {
                "unit_relative_file64_offset": table["offset"],
                "file64_bytes": len(table["file64"]),
                "file64_sha256": sha256(table["file64"]),
                "t48_bytes": len(t48),
                "t48_sha256": sha256(t48),
                "t48_fingerprint_fnv1a": f"{fnv1a(t48):016x}",
            },
            "shoulder_branches": {"left": left_slots, "right": right_slots},
            "coverage": {
                "left": "supported" if left_slots else "unsupported_no_weighted_descendant",
                "right": "supported" if right_slots else "unsupported_no_weighted_descendant",
            },
        })

    scene.pop("hashes")
    scene.pop("parents")
    return {
        "schema": "HD2RIG",
        "version": 1,
        "source": {
            "patch": patch_relative,
            "triplet": patch_triplet,
            "unit_id": f"{entry['file_id']:016x}",
            "unit_bundle_offset": entry["data_offset"],
            "unit_bytes": len(unit),
            "unit_sha256": sha256(unit),
        },
        "coordinate_frame": {
            "matrix_convention": "row_vector",
            "affine_translation_row": [12, 13, 14],
            "packed_t48_translation_indices": [3, 7, 11],
            "skin_equation": "K = B * W",
            "output_frame": "palette_skin_output_O",
            "known_per_unit_relationships": [],
            "live_relationship_status": "unverified",
        },
        "roles": roles,
        "complete_shoulder_descendants": {"left": left_nodes, "right": right_nodes},
        "scene_graph": scene,
        "lods": lods,
        "live_layout": None,
        "observer_metadata": None,
    }


def export_rig_tree(root: str, output_directory: str, role_values: list[str],
                    force: bool) -> dict:
    root = os.path.abspath(root)
    output_directory = os.path.abspath(output_directory)
    if not os.path.isdir(root):
        raise ValueError("--root is not a directory")
    configured_roles = parse_role_assignments(role_values)
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

    sidecars = []
    skipped = []
    for patch in patches:
        relative = os.path.relpath(patch, root).replace("\\", "/")
        bundle = read_file(patch)
        triplet = triplet_hashes(patch)
        suffixes = {"main": "", "gpu": ".gpu_resources", "stream": ".stream"}
        for kind, metadata in triplet.items():
            metadata["file"] = relative + suffixes[kind]
        prefix = sha256(relative.casefold().encode("utf-8"))[:16]
        for entry in bundle_entries(bundle):
            if entry["type_id"] != UNIT_TYPE:
                continue
            unit = bundle[entry["data_offset"]:entry["data_offset"] + entry["data_size"]]
            try:
                document = _build_sidecar(relative, triplet, entry, unit, configured_roles)
            except NotRig as error:
                skipped.append({"patch": relative, "unit_id": f"{entry['file_id']:016x}",
                                "reason": str(error)})
                continue
            name = f"{prefix}__{os.path.basename(patch)}__{entry['file_id']:016x}.hd2rig.json"
            sidecars.append((name, document))
    if not sidecars:
        raise ValueError("no units with both TransformInfo and BoneInfo were found")

    manifest_path = os.path.join(output_directory, "rig_sidecars_manifest.json")
    output_paths = [os.path.join(output_directory, name) for name, _ in sidecars]
    existing = [path for path in output_paths + [manifest_path] if os.path.exists(path)]
    if existing and not force:
        raise ValueError("output already exists; use --force: " + existing[0])
    os.makedirs(output_directory, exist_ok=True)
    for name, document in sidecars:
        write_atomic(os.path.join(output_directory, name),
                     (json.dumps(document, indent=2) + "\n").encode("utf-8"))
    manifest = {
        "schema": "HD2RIG-MANIFEST",
        "version": 1,
        "root": root,
        "configured_roles": {key: configured_roles.get(key) for key in ROLE_KEYS},
        "sidecars": [{"file": name, "patch": document["source"]["patch"],
                      "unit_id": document["source"]["unit_id"]}
                     for name, document in sidecars],
        "skipped_units": skipped,
    }
    write_atomic(manifest_path, (json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
    return manifest
