#!/usr/bin/env python
"""Build a dynamic, clavicle-rooted shoulder plan from HD2RIG sidecars."""

from __future__ import annotations

import json
import math
import os

from extract_runtime_profile import write_atomic


def _descendants(nodes: list[dict], root: int) -> dict[int, int]:
    parents = [node["parent"] for node in nodes]
    result = {}
    for candidate in range(len(nodes)):
        current = candidate
        depth = 0
        while current is not None:
            if current == root:
                result[candidate] = depth
                break
            current = parents[current]
            depth += 1
    return result


def _load_sidecars(directory: str) -> list[tuple[str, dict]]:
    if not os.path.isdir(directory):
        raise ValueError("--rig-dir is not a directory")
    paths = sorted(
        (os.path.join(directory, name) for name in os.listdir(directory)
         if name.endswith(".hd2rig.json")), key=str.casefold)
    if not paths:
        raise ValueError("--rig-dir contains no .hd2rig.json sidecars")
    documents = []
    for path in paths:
        with open(path, encoding="utf-8") as stream:
            document = json.load(stream)
        if document.get("schema") != "HD2RIG" or document.get("version") != 1:
            raise ValueError(os.path.basename(path) + " has an unsupported sidecar schema")
        documents.append((os.path.basename(path), document))
    return documents


def build_pose_branch_plan(rig_directory: str, output_path: str,
                           left_displacement: tuple[float, float, float],
                           right_displacement: tuple[float, float, float],
                           active_tables: set[tuple[int, int, int]] | None,
                           force: bool) -> dict:
    if (not all(math.isfinite(value) and abs(value) <= 10.0
                for value in left_displacement + right_displacement) or
            not any(value != 0.0 for value in left_displacement) or
            not any(value != 0.0 for value in right_displacement)):
        raise ValueError("output displacements must be finite, nonzero, and within +/-10 metres")

    output_path = os.path.abspath(output_path)
    if os.path.exists(output_path) and not force:
        raise ValueError("output already exists; use --force to replace it: " + output_path)

    observations: dict[tuple[int, int, int, int], dict] = {}
    table_sources: dict[tuple[int, int, int], list[dict]] = {}
    skipped_tables = []
    for sidecar_name, document in _load_sidecars(rig_directory):
        nodes = document["scene_graph"]["nodes"]
        unit_text = document["source"]["unit_id"]
        unit_id = int(unit_text, 16)
        roots = {}
        for side in ("left", "right"):
            shoulder = document["roles"][side + "_shoulder"]
            if shoulder["status"] != "resolved":
                roots[side] = None
                continue
            shoulder_node = shoulder["node"]
            clavicle_node = nodes[shoulder_node]["parent"]
            if clavicle_node is None:
                raise ValueError(f"{sidecar_name}: {side} shoulder has no structural clavicle parent")
            roots[side] = {
                "node": clavicle_node,
                "name_hash": nodes[clavicle_node]["name_hash"],
                "descendants": _descendants(nodes, clavicle_node),
            }
        if roots["left"] is not None and roots["right"] is not None and \
                roots["left"]["node"] == roots["right"]["node"]:
            raise ValueError(f"{sidecar_name}: shoulders share one parent; clavicle roots are ambiguous")
        if roots["left"] is not None and roots["right"] is not None:
            overlap = set(roots["left"]["descendants"]) & set(roots["right"]["descendants"])
            if overlap:
                raise ValueError(f"{sidecar_name}: derived clavicle branches overlap")

        for lod in document["lods"]:
            entries = lod["entry_count"]
            table_key = int(lod["table"]["t48_fingerprint_fnv1a"], 16)
            table_identity = (unit_id, table_key, entries)
            if active_tables is not None and table_identity not in active_tables:
                skipped_tables.append({
                    "sidecar": sidecar_name, "unit_id": unit_text,
                    "table_key": f"{table_key:016x}", "lod": lod["lod"],
                    "reason": "not present in active runtime profiles",
                })
                continue
            table_sources.setdefault(table_identity, []).append({
                "sidecar": sidecar_name,
                "patch": document["source"]["patch"],
                "patch_main_sha256": document["source"]["triplet"]["main"]["sha256"],
                "unit_sha256": document["source"]["unit_sha256"],
                "lod": lod["lod"],
            })
            for slot, node in enumerate(lod["slot_to_scene_node"]):
                memberships = [side for side in ("left", "right")
                               if roots[side] is not None and
                               node in roots[side]["descendants"]]
                if not memberships:
                    continue
                if len(memberships) != 1:
                    raise ValueError(f"{sidecar_name}: slot {slot} belongs to both clavicle branches")
                side = memberships[0]
                root = roots[side]
                displacement = left_displacement if side == "left" else right_displacement
                key = (unit_id, table_key, entries, slot)
                value = {
                    "unit_id": unit_text,
                    "table_key": f"{table_key:016x}",
                    "entry_count": entries,
                    "slot": slot,
                    "side": side,
                    "scene_node": node,
                    "clavicle_root_node": root["node"],
                    "clavicle_root_hash": root["name_hash"],
                    "depth_from_clavicle": root["descendants"][node],
                    "output_displacement_m": list(displacement),
                    "sources": [],
                }
                existing = observations.get(key)
                semantic = ("side", "scene_node", "clavicle_root_hash",
                            "depth_from_clavicle", "output_displacement_m")
                if existing is not None and any(existing[field] != value[field] for field in semantic):
                    raise ValueError(
                        f"unit {unit_text} table {table_key:016x} slot {slot} has conflicting rig semantics")
                target = observations.setdefault(key, value)
                target["sources"].append({"sidecar": sidecar_name, "lod": lod["lod"]})

    tables = []
    grouped: dict[tuple[int, int, int], list[dict]] = {}
    for key, target in sorted(observations.items()):
        grouped.setdefault(key[:3], []).append(target)
    for identity, slots in sorted(grouped.items()):
        tables.append({
            "unit_id": f"{identity[0]:016x}",
            "table_key": f"{identity[1]:016x}",
            "entry_count": identity[2],
            "slots": slots,
            "sources": table_sources[identity],
        })
    if not tables:
        raise ValueError("no active weighted clavicle descendants were found")

    plan = {
        "schema": "HD2POSE-BRANCH-PLAN",
        "version": 1,
        "operation": "rigid_output_translation",
        "coordinate_frame": "palette_skin_output_O",
        "branch_root": "structural_parent_of_shoulder",
        "falloff": None,
        "requested_output_displacement_m": {
            "left": list(left_displacement), "right": list(right_displacement),
        },
        "requirements": ["SkinLinearBlocks", "VerifiedInstanceAssociation"],
        "runtime_status": "offline_plan_only",
        "tables": tables,
        "table_count": len(tables),
        "slot_count": sum(len(table["slots"]) for table in tables),
        "slots_by_side": {
            side: sum(slot["side"] == side for table in tables for slot in table["slots"])
            for side in ("left", "right")
        },
        "skipped_tables": skipped_tables,
    }
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    write_atomic(output_path, (json.dumps(plan, indent=2) + "\n").encode("utf-8"))
    return plan
