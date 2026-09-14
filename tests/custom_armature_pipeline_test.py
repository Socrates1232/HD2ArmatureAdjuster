#!/usr/bin/env python
"""Verify deterministic source export, porter semantics, validation, and replay."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

from rig_sidecar_pipeline_test import semantic_bundle


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from rig_profile_tool import (build_source_reference, canonical_bytes, package_rig, read_json,
                              replay, sha256, validate_rig_document)


spec = importlib.util.spec_from_file_location(
    "hd2_porter_core", ROOT / "tools" / "blender" / "hd2_armature_porter" / "core.py")
porter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(porter)


def reject(action, message):
    try:
        action()
    except ValueError as error:
        if message not in str(error):
            raise AssertionError(f"wrong rejection: {error}") from error
    else:
        raise AssertionError("invalid package was accepted")


def main() -> int:
    if porter.bone_name_hash("l_clavicle") != 0xC4787B4E:
        raise AssertionError("Blender bone-name hashing does not match HD2")
    if porter.armature_name_hash("3365605331") != 0xC89B0FD3:
        raise AssertionError("decimal Blender bone hashes are not preserved")
    aliased_parent = {"node_c4787b4e_variant": {"name_hash": "c4787b4e"}}
    if not porter.parent_matches(aliased_parent, "node_c4787b4e_variant", "l_clavicle"):
        raise AssertionError("a named Blender parent did not match its structural source alias")
    if porter.parent_matches(aliased_parent, "node_c4787b4e_variant", "spine_2"):
        raise AssertionError("a genuinely different target parent was accepted")
    decimal_parent = {"node_c89b0fd3": {"name_hash": "c89b0fd3"}}
    if not porter.parent_matches(decimal_parent, "node_c89b0fd3", "3365605331"):
        raise AssertionError("a decimal Blender parent did not match its HD2 hash")
    variant_source = {
        "bones": [
            {"stable_id": "root_a", "parent_id": None, "name_hash": "c89b0fd3"},
            {"stable_id": "root_b", "parent_id": None, "name_hash": "582bc7a5"},
            {"stable_id": "mesh_a", "parent_id": "root_a", "name_hash": "315cd161"},
            {"stable_id": "mesh_b", "parent_id": "root_b", "name_hash": "315cd161"},
            {"stable_id": "shoulder", "parent_id": "root_a", "name_hash": "03752c98"},
        ],
        "tables": [{"slots": [{"source_bone_id": "shoulder"}]}],
    }
    if porter.runtime_dependency_ids(variant_source) != {"root_a", "shoulder"}:
        raise AssertionError("non-skinning structural variants entered the runtime closure")
    compatible = porter.matching_parent_records(
        {bone["stable_id"]: bone for bone in variant_source["bones"]},
        variant_source["bones"][2:4], "3365605331")
    if [record["stable_id"] for record in compatible] != ["mesh_a"]:
        raise AssertionError("union variants were not qualified by their parent identity")

    with tempfile.TemporaryDirectory() as temporary:
        root = pathlib.Path(temporary) / "mod"
        root.mkdir()
        patch = root / "0123456789abcdef.patch_0"
        patch.write_bytes(semantic_bundle())
        profiles = root / "HD2ArmatureProfiles"
        profiles.mkdir()
        profile = profiles / "synthetic.hd2profile"
        command = [sys.executable, str(ROOT / "tools" / "extract_runtime_profile.py"),
                   "--patch", str(patch), "--out", str(profile)]
        completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        if completed.returncode:
            raise AssertionError(completed.stdout)
        (profiles / "active_profiles.txt").write_text(profile.name + "\n", encoding="utf-8")

        roles = ["chest=spine_2", "left_elbow=l_elbow", "right_elbow=r_elbow",
                 "left_hand=l_hand", "right_hand=r_hand"]
        source = build_source_reference(str(root), str(profiles), roles)
        source_path = pathlib.Path(temporary) / "source.hd2source.json"
        source_path.write_bytes(canonical_bytes(source))
        porter_zip = pathlib.Path(temporary) / "porter.zip"
        command = [sys.executable, str(ROOT / "tools" / "build_blender_porter.py"),
                   "--source", str(source_path), "--out", str(porter_zip)]
        completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        if completed.returncode:
            raise AssertionError(completed.stdout)
        with zipfile.ZipFile(porter_zip) as archive:
            if "source.hd2source.json" not in archive.namelist():
                raise AssertionError("Blender porter omitted its bundled source contract")
            bundled = json.loads(archive.read("source.hd2source.json"))
            if bundled["source_definition_id"] != source["source_definition_id"]:
                raise AssertionError("Blender porter bundled the wrong source contract")
        second = build_source_reference(str(root), str(profiles), roles)
        if canonical_bytes(source) != canonical_bytes(second):
            raise AssertionError("source reference export is not deterministic")

        local = {bone["stable_id"]: bone["source_rest_local"] for bone in source["bones"]}
        parents = {bone["stable_id"]: bone["parent_id"] for bone in source["bones"]}
        identity_rig = porter.build_rig(str(source_path), local, parents)
        if identity_rig["requested_capability"] != "linear_rest_translation":
            raise AssertionError("identity target did not choose the linear path")
        first_bytes = porter.canonical_bytes(identity_rig)
        if first_bytes != porter.canonical_bytes(porter.build_rig(
                str(source_path), local, parents)):
            raise AssertionError("rig package export is not deterministic")

        rig_path = pathlib.Path(temporary) / "target.hd2rig.json"
        rig_path.write_bytes(first_bytes)
        report = validate_rig_document(identity_rig, str(profiles), str(source_path))
        if report["status"] != "VALID" or report["bones"] != 8 or report["tables"] != 2:
            raise AssertionError("identity package validation returned the wrong coverage")
        package_directory = pathlib.Path(temporary) / "HD2ArmatureRigs"
        manifest = package_rig(str(rig_path), str(source_path), str(profiles),
                               str(package_directory), False)
        if manifest["schema"] != "HD2AAPACKAGE1" or \
                (package_directory / "active_rig.txt").read_text(encoding="utf-8") != \
                "target.hd2rig.json\nsource.hd2source.json\n":
            raise AssertionError("install package was not assembled correctly")
        reject(lambda: package_rig(str(rig_path), str(source_path), str(profiles),
                                   str(package_directory), False), "already exists")

        left = next(bone for bone in source["bones"] if "left_shoulder" in bone["roles"])
        target = copy.deepcopy(local)
        target[left["stable_id"]][3] += 0.06
        moved = porter.build_rig(str(source_path), target, parents)
        if not moved["capability_report"]["linear_path_eligible"]:
            raise AssertionError("translation-only target did not remain linear eligible")
        native = {bone["stable_id"]: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                                              0.0, 0.0, 1.0]
                  for bone in moved["bones"]}
        result = replay(moved, {"source_sample_id": "fixture:1", "native_linear": native},
                        str(profiles))
        left_branch = set()
        cursor = {bone["stable_id"]: bone for bone in source["bones"]}
        for bone in source["bones"]:
            parent = bone["stable_id"]
            while parent is not None:
                if parent == left["stable_id"]:
                    left_branch.add(bone["stable_id"])
                    break
                parent = cursor[parent]["parent_id"]
        for stable_id in left_branch:
            if abs(result["bone_displacements"][stable_id][0] - 0.06) > 1e-6:
                raise AssertionError("descendant displacement did not propagate uniformly")

        scripted_path = pathlib.Path(temporary) / "scripted.hd2rig.json"
        command = [sys.executable, str(ROOT / "tools" / "translate_rig_roles.py"),
                   "--source", str(source_path), "--out", str(scripted_path),
                   "--role", "left_shoulder", "0.06", "0", "0"]
        completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        if completed.returncode:
            raise AssertionError(completed.stdout)
        scripted = read_json(str(scripted_path))
        if validate_rig_document(scripted, str(profiles), str(source_path))["status"] != "VALID":
            raise AssertionError("scripted semantic-role porter output is invalid")

        broken_parent = dict(parents)
        broken_parent[left["stable_id"]] = None
        reject(lambda: porter.build_rig(str(source_path), local, broken_parent),
               "hierarchy differs")
        rotated = copy.deepcopy(local)
        rotated[left["stable_id"]][0] = 0.5
        reject(lambda: porter.build_rig(str(source_path), rotated, parents,
                                        requested_capability="linear_rest_translation"),
               "requires preserved rest bases")
        stale = copy.deepcopy(identity_rig)
        stale["source_reference_sha256"] = "0" * 64
        reject(lambda: validate_rig_document(stale, str(profiles), str(source_path)),
               "source reference hash")
        stale_profile = copy.deepcopy(identity_rig)
        stale_profile["tables"][0]["profile_dependencies"][0]["sha256"] = "0" * 64
        reject(lambda: validate_rig_document(stale_profile, str(profiles), str(source_path)),
               "profile dependencies are stale")
        fixture = copy.deepcopy(identity_rig)
        fixture["fixture_only"] = True
        reject(lambda: validate_rig_document(fixture, str(profiles), None), "fixture_only")

        duplicate = pathlib.Path(temporary) / "duplicate.json"
        duplicate.write_text('{"schema":"HD2RIG1","schema":"bad"}', encoding="utf-8")
        reject(lambda: read_json(str(duplicate)), "duplicate JSON key")
        if sha256(source_path.read_bytes()) != identity_rig["source_reference_sha256"]:
            raise AssertionError("rig did not pin the exact source reference")

    print("custom armature source, porter, validator, and replay pipeline passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
