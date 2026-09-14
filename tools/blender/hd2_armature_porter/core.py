"""Blender-independent HD2 custom armature package builder."""

from __future__ import annotations

import hashlib
import json
import math
import pathlib
import struct


IDENTITY = [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]
MASK64 = 0xFFFFFFFFFFFFFFFF


def bone_name_hash(name: str) -> int:
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


def armature_name_hash(name: str) -> int:
    if name.isdecimal():
        value = int(name)
        if value <= 0xFFFFFFFF:
            return value
    return bone_name_hash(name)


def parent_matches(source_by_id: dict, expected_parent_id: str | None,
                   actual_parent_name: str | None,
                   actual_parent_id: str | None = None) -> bool:
    if expected_parent_id is None:
        return actual_parent_name is None
    if actual_parent_id == expected_parent_id:
        return True
    if actual_parent_name is None:
        return False
    expected_hash = source_by_id[expected_parent_id]["name_hash"].lower()
    return f"{armature_name_hash(actual_parent_name):08x}" == expected_hash


def canonical_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def file_sha256(path: str) -> str:
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def read_source(path: str) -> dict:
    source = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    if source.get("schema") != "HD2SOURCE1":
        raise ValueError("source reference is not HD2SOURCE1")
    if source.get("matrix_convention") != "column_vectors_row_major_storage":
        raise ValueError("source reference uses an unsupported matrix convention")
    return source


def matrix16(value: object, label: str) -> list[float]:
    if not isinstance(value, (list, tuple)) or len(value) != 16:
        raise ValueError(label + " must contain 16 numbers")
    result = [float(item) for item in value]
    if not all(math.isfinite(item) for item in result):
        raise ValueError(label + " contains a non-finite value")
    if max(abs(result[index] - expected) for index, expected in
           zip((12, 13, 14, 15), (0.0, 0.0, 0.0, 1.0))) > 1e-5:
        raise ValueError(label + " is not a column-vector affine matrix")
    return result


def _linear(matrix: list[float]) -> list[float]:
    return [matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6],
            matrix[8], matrix[9], matrix[10]]


def _position(matrix: list[float]) -> list[float]:
    return [matrix[3], matrix[7], matrix[11]]


def _with_position(linear_source: list[float], target: list[float]) -> list[float]:
    result = list(linear_source)
    result[3], result[7], result[11] = _position(target)
    return result


def build_rig(source_path: str, target_local: dict[str, list[float]],
              target_parents: dict[str, str | None], basis_mode: str = "preserved",
              requested_capability: str = "auto", notes: str = "") -> dict:
    source = read_source(source_path)
    source_bones = source.get("bones", [])
    if not source_bones:
        raise ValueError("source reference contains no bones")
    source_ids = {bone["stable_id"] for bone in source_bones}
    missing = sorted(source_ids - set(target_local))
    unknown = sorted(set(target_local) - source_ids)
    if missing:
        raise ValueError("target is missing stable bones: " + ", ".join(missing[:12]))
    if unknown:
        raise ValueError("target contains unknown stable bones: " + ", ".join(unknown[:12]))
    if basis_mode not in ("preserved", "position_normalized", "exact_full"):
        raise ValueError("unsupported authoring basis mode")

    bones = []
    reasons = []
    for source_bone in source_bones:
        stable_id = source_bone["stable_id"]
        expected_parent = source_bone["parent_id"]
        if target_parents.get(stable_id) != expected_parent:
            raise ValueError(stable_id + ": target hierarchy differs from the source")
        source_local = matrix16(source_bone["source_rest_local"], stable_id + " source rest")
        edited_local = matrix16(target_local[stable_id], stable_id + " target rest")
        target = (_with_position(source_local, edited_local)
                  if basis_mode == "position_normalized" else edited_local)
        if expected_parent is None and max(abs(a - b) for a, b in zip(source_local, target)) > 1e-6:
            raise ValueError(stable_id + ": root edits are forbidden by preserve_native")
        if max(abs(a - b) for a, b in zip(_linear(source_local), _linear(target))) > 1e-6:
            reasons.append(stable_id + ": rest linear basis changed")
        output = {
            "stable_id": stable_id,
            "display_name": source_bone.get("display_name", stable_id),
            "parent_id": expected_parent,
            "source_rest_local": source_local,
            "target_rest_local": target,
            "delta_basis": list(IDENTITY),
        }
        roles = source_bone.get("roles", [])
        if roles:
            output["role"] = ",".join(sorted(roles))
        bones.append(output)

    linear_eligible = not reasons
    capability = ("linear_rest_translation" if linear_eligible else "full_native_pose"
                  if requested_capability == "auto" else requested_capability)
    if requested_capability != "auto":
        capability = requested_capability
    if capability == "linear_rest_translation" and not linear_eligible:
        raise ValueError("linear_rest_translation requires preserved rest bases")
    if capability not in ("linear_rest_translation", "full_native_pose"):
        raise ValueError("unsupported requested capability")

    tables = []
    for source_table in source.get("tables", []):
        slots = [{"slot": slot["slot"],
                  "source_bone_id": slot["source_bone_id"],
                  "target_bone_id": slot["source_bone_id"]}
                 for slot in source_table["slots"]]
        tables.append({
            "unit_id": source_table["unit_id"],
            "table_sha256": source_table["table_sha256"],
            "entries": source_table["entries"],
            "lod_mask": source_table["lod_mask"],
            "profile_dependencies": source_table["profile_dependencies"],
            "pose_space_adapter_id": source_table["pose_space_adapter_id"],
            "slots": slots,
        })

    rig = {
        "schema": "HD2RIG1",
        "rig_id": "pending",
        "source_reference_sha256": file_sha256(source_path),
        "matrix_convention": "column_vectors_row_major_storage",
        "unit": "metre",
        "retarget_policy": "copy_effective_local_delta",
        "binding_mode": "reshape_existing",
        "requested_capability": capability,
        "root_policy": "preserve_native",
        "bones": bones,
        "tables": tables,
        "space_adapters": source.get("space_adapters", [{"id": "common_identity", "mode": "identity"}]),
        "capability_report": {"linear_path_eligible": linear_eligible, "reasons": reasons},
        "authoring": {"basis_mode": basis_mode, "normalization_confirmed": basis_mode == "position_normalized",
                      "notes": notes},
    }
    identity = dict(rig)
    identity.pop("rig_id")
    rig["rig_id"] = hashlib.sha256(canonical_bytes(identity)).hexdigest()
    return rig


def write_rig(path: str, rig: dict, force: bool = False) -> str:
    destination = pathlib.Path(path)
    if destination.exists() and not force:
        raise ValueError("output already exists; enable overwrite explicitly")
    data = canonical_bytes(rig)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(destination)
    return hashlib.sha256(data).hexdigest()
