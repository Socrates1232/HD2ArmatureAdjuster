#!/usr/bin/env python
"""Export, validate, and replay HD2 custom-armature packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys
import tempfile

from patch_profile_tool import profile_runtime_keys, refuse_game_output
from rig_sidecar import export_rig_tree
from retarget_math import (IDENTITY, affine, encode_t48, encode_translation, linear, position,
                           propagate_displacements, rest_deltas)


MAX_JSON_BYTES = 16 * 1024 * 1024
HEX16 = __import__("re").compile(r"^[0-9a-f]{16}$")
HEX64 = __import__("re").compile(r"^[0-9a-f]{64}$")


def canonical_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json(path: str) -> dict:
    data = pathlib.Path(path).read_bytes()
    if len(data) > MAX_JSON_BYTES:
        raise ValueError("JSON exceeds the 16 MiB package limit")

    def unique(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    value = json.loads(data, object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise ValueError("package root must be an object")
    return value


def write_json(path: str, value: object, force: bool) -> str:
    path = os.path.abspath(path)
    refuse_game_output(path)
    if os.path.exists(path) and not force:
        raise ValueError("output already exists; use --force: " + path)
    data = canonical_bytes(value)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    temporary = path + ".tmp"
    with open(temporary, "wb") as stream:
        stream.write(data)
    os.replace(temporary, path)
    return sha256(data)


def transpose(values: list[float]) -> list[float]:
    if len(values) != 16:
        raise ValueError("matrix must have 16 values")
    return [float(values[column * 4 + row]) for row in range(4) for column in range(4)]


def profile_registry(directory: str) -> tuple[dict, list[dict]]:
    active_path = os.path.join(directory, "active_profiles.txt")
    if not os.path.isfile(active_path):
        raise ValueError("profile directory has no active_profiles.txt")
    names = [line.strip() for line in pathlib.Path(active_path).read_text(
        encoding="utf-8").splitlines() if line.strip()]
    registry: dict[tuple[int, str, int], list[dict]] = {}
    packages = []
    for name in names:
        if os.path.basename(name) != name or not name.endswith(".hd2profile"):
            raise ValueError("active_profiles.txt contains an invalid filename")
        path = os.path.join(directory, name)
        digest = sha256(pathlib.Path(path).read_bytes())
        packages.append({"filename": name, "sha256": digest})
        for unit_id, entries, table in profile_runtime_keys(path):
            registry.setdefault((unit_id, sha256(table), entries), []).append(
                {"filename": name, "sha256": digest})
    if not registry:
        raise ValueError("active profiles contain no runtime tables")
    return registry, packages


def _topological_bones(records: dict[str, dict]) -> list[dict]:
    pending = dict(records)
    ordered = []
    emitted = set()
    while pending:
        ready = sorted(key for key, value in pending.items()
                       if value["parent_id"] is None or value["parent_id"] in emitted)
        if not ready:
            raise ValueError("source graph contains a cycle or missing parent")
        for key in ready:
            ordered.append(pending.pop(key))
            emitted.add(key)
    return ordered


def build_source_reference(root: str, profiles: str, role_values: list[str]) -> dict:
    registry, profile_packages = profile_registry(profiles)
    with tempfile.TemporaryDirectory() as temporary:
        manifest = export_rig_tree(root, temporary, role_values, False)
        sidecars = [(item["file"], read_json(os.path.join(temporary, item["file"])))
                    for item in manifest["sidecars"]]

    appearances = []
    appearance_ids = {}
    for filename, sidecar in sidecars:
        nodes = sidecar["scene_graph"]["nodes"]
        hashes = [node["name_hash"] for node in nodes]
        if len(set(hashes)) != len(hashes):
            raise ValueError(f"{filename}: duplicate scene-node hashes require explicit identities")
        matrices = []
        for node in nodes:
            local = node["local_transform"]
            row_local = [
                *local["rotation"][0], 0.0, *local["rotation"][1], 0.0,
                *local["rotation"][2], 0.0, *local["position"], 1.0,
            ]
            for row in range(3):
                for column in range(3):
                    row_local[row * 4 + column] *= local["scale"][row]
            source_local = transpose(row_local)
            source_global = transpose(node["stored_rest_matrix"])
            matrices.append((source_local, source_global))

        variants = {}

        def structural_variant(index: int) -> str:
            if index in variants:
                return variants[index]
            node = nodes[index]
            parent = node["parent"]
            source_local, source_global = matrices[index]
            structural = {
                "name_hash": node["name_hash"],
                "parent_variant": None if parent is None else structural_variant(parent),
                "local": [round(value, 5) for value in source_local],
                "global": [round(value, 5) for value in source_global],
            }
            variants[index] = sha256(canonical_bytes(structural))[:12]
            return variants[index]

        for node in nodes:
            source_local, source_global = matrices[node["index"]]
            appearances.append({
                "key": (filename, node["index"]),
                "filename": filename,
                "sidecar": sidecar,
                "node": node,
                "source_local": source_local,
                "source_global": source_global,
                "variant": structural_variant(node["index"]),
            })

    variants_by_hash: dict[str, set[str]] = {}
    for item in appearances:
        variants_by_hash.setdefault(item["node"]["name_hash"], set()).add(item["variant"])
    for item in appearances:
        name_hash = item["node"]["name_hash"]
        stable_id = "node_" + name_hash
        if len(variants_by_hash[name_hash]) > 1:
            stable_id += "_" + item["variant"]
        appearance_ids[item["key"]] = stable_id

    bones: dict[str, dict] = {}
    tables: dict[tuple[int, str, int], dict] = {}
    patch_triplets = {}
    maximum_rest_residual = 0.0
    for item in appearances:
        filename = item["filename"]
        sidecar = item["sidecar"]
        nodes = sidecar["scene_graph"]["nodes"]
        node = item["node"]
        stable_id = appearance_ids[item["key"]]
        parent = node["parent"]
        parent_id = None if parent is None else appearance_ids[(filename, parent)]
        source_local = item["source_local"]
        source_global = item["source_global"]
        display = node["names"][0] if node["names"] else stable_id
        candidate = {
            "stable_id": stable_id, "display_name": display,
            "name_hash": node["name_hash"], "parent_id": parent_id,
            "source_rest_local": source_local,
            "source_rest_global": source_global,
            "roles": [], "origins": [],
        }
        for role, value in sidecar["roles"].items():
            if value["status"] == "resolved" and value["node"] == node["index"]:
                candidate["roles"].append(role)
        candidate["origins"].append({"sidecar": filename,
                                     "unit_id": sidecar["source"]["unit_id"],
                                     "node": node["index"]})
        existing = bones.get(stable_id)
        if existing is None:
            bones[stable_id] = candidate
        else:
            if existing["parent_id"] != parent_id:
                raise ValueError(f"{stable_id}: unresolved structural identity collision")
            residual = max(abs(a - b) for a, b in zip(
                existing["source_rest_local"], source_local))
            maximum_rest_residual = max(maximum_rest_residual, residual)
            if residual > 2e-5:
                raise ValueError(f"{stable_id}: conflicting rest transforms ({residual:g})")
            existing["origins"].extend(candidate["origins"])
            existing["roles"] = sorted(set(existing["roles"] + candidate["roles"]))
            if existing["display_name"].startswith("node_") and not display.startswith("node_"):
                existing["display_name"] = display

    for filename, sidecar in sidecars:
        nodes = sidecar["scene_graph"]["nodes"]
        node_ids = [appearance_ids[(filename, node["index"])] for node in nodes]
        patch = sidecar["source"]["patch"]
        patch_triplets.setdefault(patch, sidecar["source"]["triplet"])
        unit_id = int(sidecar["source"]["unit_id"], 16)
        for lod in sidecar["lods"]:
            table_sha = lod["table"]["t48_sha256"]
            entries = lod["entry_count"]
            key = (unit_id, table_sha, entries)
            if key not in registry:
                continue
            slots = [{"slot": slot, "source_bone_id": node_ids[node]}
                     for slot, node in enumerate(lod["slot_to_scene_node"])]
            candidate = {
                "unit_id": f"{unit_id:016x}", "table_sha256": table_sha,
                "table_fingerprint_fnv1a": lod["table"]["t48_fingerprint_fnv1a"],
                "entries": entries, "lod_mask": 1 << lod["lod"],
                "profile_dependencies": registry[key],
                "pose_space_adapter_id": "common_identity", "slots": slots,
                "origins": [{"sidecar": filename, "patch": patch, "lod": lod["lod"]}],
            }
            existing = tables.get(key)
            if existing is None:
                tables[key] = candidate
            else:
                if existing["slots"] != slots:
                    raise ValueError(
                        f"unit {unit_id:016x} table {table_sha}: conflicting slot semantics")
                existing["lod_mask"] |= 1 << lod["lod"]
                existing["origins"].extend(candidate["origins"])

    ordered_bones = _topological_bones(bones)
    source = {
        "schema": "HD2SOURCE1", "matrix_convention": "column_vectors_row_major_storage",
        "unit": "metre", "source_root": os.path.basename(os.path.normpath(root)),
        "profile_packages": profile_packages,
        "patch_triplets": [{"patch": key, "triplet": patch_triplets[key]}
                           for key in sorted(patch_triplets, key=str.casefold)],
        "bones": ordered_bones,
        "tables": sorted(tables.values(), key=lambda item: (
            item["unit_id"], item["table_sha256"], item["entries"])),
        "space_adapters": [{"id": "common_identity", "mode": "identity"}],
        "validation": {
            "source_rest_interpretation": "TransformData local and stored rest matrices",
            "rest_equation_max_absolute_error": maximum_rest_residual,
            "active_table_count": len(tables),
            "structural_hash_collision_count": sum(
                len(variants) - 1 for variants in variants_by_hash.values()),
        },
    }
    identity = dict(source)
    identity.pop("source_root")
    source["source_definition_id"] = sha256(canonical_bytes(identity))
    return source


def validate_rig_document(rig: dict, profiles: str, source_path: str | None,
                          allow_fixture: bool = False) -> dict:
    required = {"schema", "rig_id", "source_reference_sha256", "matrix_convention",
                "unit", "retarget_policy", "binding_mode", "requested_capability",
                "root_policy", "bones", "tables", "space_adapters", "capability_report"}
    missing = sorted(required - set(rig))
    if missing:
        raise ValueError("rig is missing: " + ", ".join(missing))
    if rig["schema"] != "HD2RIG1" or rig["matrix_convention"] != \
            "column_vectors_row_major_storage" or rig["unit"] != "metre":
        raise ValueError("unsupported rig schema, matrix convention, or unit")
    if rig.get("fixture_only", False) and not allow_fixture:
        raise ValueError("fixture_only rigs are not installable")
    if not HEX64.fullmatch(rig["source_reference_sha256"]):
        raise ValueError("source_reference_sha256 is invalid")
    if source_path:
        actual = sha256(pathlib.Path(source_path).read_bytes())
        if actual != rig["source_reference_sha256"]:
            raise ValueError("source reference hash does not match the rig")
    if rig["retarget_policy"] != "copy_effective_local_delta" or \
            rig["root_policy"] != "preserve_native":
        raise ValueError("unsupported retarget or root policy")
    if rig["binding_mode"] not in ("reshape_existing", "target_bound"):
        raise ValueError("unsupported binding mode")
    if not 1 <= len(rig["bones"]) <= 4096 or not 1 <= len(rig["tables"]) <= 4096:
        raise ValueError("rig exceeds bone/table budgets")

    ids = [bone["stable_id"] for bone in rig["bones"]]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate stable bone IDs")
    by_id = {bone["stable_id"]: bone for bone in rig["bones"]}
    parents = []
    index_by_id = {}
    linear_eligible = True
    reasons = []
    for index, bone in enumerate(rig["bones"]):
        if set(bone) - {"stable_id", "display_name", "parent_id", "source_rest_local",
                       "target_rest_local", "delta_basis", "role"}:
            raise ValueError(f"{bone['stable_id']}: unknown bone field")
        parent = bone["parent_id"]
        if parent is not None and parent not in index_by_id:
            raise ValueError(f"{bone['stable_id']}: parent is missing or ordered after child")
        parents.append(-1 if parent is None else index_by_id[parent])
        index_by_id[bone["stable_id"]] = index
        source = affine(bone["source_rest_local"], bone["stable_id"] + " source rest")
        target = affine(bone["target_rest_local"], bone["stable_id"] + " target rest")
        basis = affine(bone["delta_basis"], bone["stable_id"] + " delta basis")
        if parent is None and max(abs(a - b) for a, b in zip(source, target)) > 1e-6:
            raise ValueError(f"{bone['stable_id']}: root rest changed")
        if max(abs(a - b) for a, b in zip(linear(source), linear(target))) > 1e-6:
            linear_eligible = False
            reasons.append(bone["stable_id"] + ": rest linear basis changed")
        if max(abs(a - b) for a, b in zip(basis, IDENTITY)) > 1e-6:
            linear_eligible = False
            reasons.append(bone["stable_id"] + ": delta basis is non-identity")

    registry, _ = profile_registry(profiles)
    table_slots = 0
    seen_tables = set()
    for table in rig["tables"]:
        unit = table["unit_id"]
        table_sha = table["table_sha256"]
        entries = table["entries"]
        if not HEX16.fullmatch(unit) or not HEX64.fullmatch(table_sha) or \
                not isinstance(entries, int) or entries < 1:
            raise ValueError("invalid table identity")
        key = (int(unit, 16), table_sha, entries)
        if key in seen_tables:
            raise ValueError("duplicate rig table identity")
        seen_tables.add(key)
        if key not in registry:
            raise ValueError(f"rig table {unit}/{table_sha} is absent from active profiles")
        slots = table["slots"]
        slot_numbers = [slot["slot"] for slot in slots]
        if len(slot_numbers) != len(set(slot_numbers)):
            raise ValueError("duplicate slot in rig table")
        for slot in slots:
            if slot["slot"] < 0 or slot["slot"] >= entries:
                raise ValueError("rig slot is outside its table")
            if slot["source_bone_id"] not in by_id or slot["target_bone_id"] not in by_id:
                raise ValueError("rig slot references an unknown bone")
            if rig["binding_mode"] == "target_bound" and "desired_bind" not in slot:
                raise ValueError("target_bound slot has no desired_bind")
        table_slots += len(slots)

    requested = rig["requested_capability"]
    if requested == "linear_rest_translation" and not linear_eligible:
        raise ValueError("REST_LINEAR_CHANGE_REQUIRES_FULL_POSE: " + "; ".join(reasons))
    if requested not in ("linear_rest_translation", "full_native_pose"):
        raise ValueError("unsupported requested capability")
    declared = rig["capability_report"]
    if declared["linear_path_eligible"] != linear_eligible:
        raise ValueError("exported capability report disagrees with semantic validation")

    return {
        "status": "VALID", "rig_id": rig["rig_id"], "bones": len(ids),
        "tables": len(rig["tables"]), "slots": table_slots,
        "binding_mode": rig["binding_mode"], "requested_capability": requested,
        "linear_path_eligible": linear_eligible, "eligibility_reasons": reasons,
        "source_reference_verified": source_path is not None,
    }


def replay(rig: dict, capture: dict, profiles: str) -> dict:
    validation = validate_rig_document(rig, profiles, None,
                                       allow_fixture=rig.get("fixture_only", False))
    if rig["requested_capability"] != "linear_rest_translation":
        raise ValueError("replay currently requires linear_rest_translation")
    bones = rig["bones"]
    index = {bone["stable_id"]: i for i, bone in enumerate(bones)}
    parents = [-1 if bone["parent_id"] is None else index[bone["parent_id"]]
               for bone in bones]
    native_by_id = capture.get("native_linear")
    if not isinstance(native_by_id, dict):
        raise ValueError("capture has no native_linear object")
    native = []
    missing = []
    for bone in bones:
        value = native_by_id.get(bone["stable_id"])
        if value is None:
            missing.append(bone["stable_id"])
        elif len(value) != 9 or not all(math.isfinite(item) for item in value):
            raise ValueError("capture contains an invalid native linear matrix")
        else:
            native.append([float(item) for item in value])
    if missing:
        raise ValueError("capture is missing required bones: " + ", ".join(missing))
    source = [bone["source_rest_local"] for bone in bones]
    target = [bone["target_rest_local"] for bone in bones]
    displacements = propagate_displacements(native, rest_deltas(source, target), parents)
    encoded_slots = 0
    for table in rig["tables"]:
        for slot in table["slots"]:
            bone_index = index[slot["source_bone_id"]]
            bind = slot.get("desired_bind", IDENTITY)
            encode_t48(encode_translation(
                native[bone_index], displacements[bone_index], bind))
            encoded_slots += 1
    return {
        "schema": "HD2REPLAY1", "status": "PLAN_BUILT",
        "rig_id": rig["rig_id"], "source_sample_id": capture.get("source_sample_id"),
        "validation": validation, "encoded_slots": encoded_slots,
        "bone_displacements": {bone["stable_id"]: displacements[i]
                               for i, bone in enumerate(bones)},
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    source = commands.add_parser("source-export")
    source.add_argument("--root", required=True)
    source.add_argument("--profiles", required=True)
    source.add_argument("--out", required=True)
    source.add_argument("--role", action="append", default=[])
    source.add_argument("--force", action="store_true")
    validate = commands.add_parser("validate")
    validate.add_argument("--rig", required=True)
    validate.add_argument("--profiles", required=True)
    validate.add_argument("--source")
    validate.add_argument("--report")
    validate.add_argument("--allow-fixture", action="store_true")
    replay_command = commands.add_parser("replay")
    replay_command.add_argument("--rig", required=True)
    replay_command.add_argument("--capture", required=True)
    replay_command.add_argument("--profiles", required=True)
    replay_command.add_argument("--out", required=True)
    replay_command.add_argument("--force", action="store_true")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "source-export":
            value = build_source_reference(args.root, args.profiles, args.role)
            digest = write_json(args.out, value, args.force)
            print(json.dumps({"output": os.path.abspath(args.out), "sha256": digest,
                              "bones": len(value["bones"]),
                              "tables": len(value["tables"])}, indent=2))
        elif args.command == "validate":
            value = validate_rig_document(read_json(args.rig), args.profiles,
                                          args.source, args.allow_fixture)
            if args.report:
                write_json(args.report, value, True)
            print(json.dumps(value, indent=2))
        else:
            value = replay(read_json(args.rig), read_json(args.capture), args.profiles)
            digest = write_json(args.out, value, args.force)
            print(json.dumps({"output": os.path.abspath(args.out), "sha256": digest,
                              "encoded_slots": value["encoded_slots"]}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print("error:", error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
