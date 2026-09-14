#!/usr/bin/env python
"""Create a translation-only HD2RIG1 target from semantic source roles."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parent
CORE_PATH = ROOT / "blender" / "hd2_armature_porter" / "core.py"
SPEC = importlib.util.spec_from_file_location("hd2_armature_porter_core", CORE_PATH)
porter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(porter)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--role", action="append", nargs=4,
                        metavar=("NAME", "X", "Y", "Z"), required=True)
    parser.add_argument("--notes", default="")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    source = porter.read_source(args.source)
    local = {bone["stable_id"]: list(bone["source_rest_local"])
             for bone in source["bones"]}
    parents = {bone["stable_id"]: bone["parent_id"] for bone in source["bones"]}
    edits = []
    for role, x, y, z in args.role:
        delta = (float(x), float(y), float(z))
        matching = [bone for bone in source["bones"] if role in bone.get("roles", [])]
        if not matching:
            raise ValueError("source reference has no semantic role: " + role)
        for bone in matching:
            matrix = local[bone["stable_id"]]
            matrix[3] += delta[0]
            matrix[7] += delta[1]
            matrix[11] += delta[2]
            edits.append({"role": role, "stable_id": bone["stable_id"], "delta": delta})

    rig = porter.build_rig(args.source, local, parents, notes=args.notes)
    digest = porter.write_rig(args.out, rig, args.force)
    print(json.dumps({"output": str(pathlib.Path(args.out).resolve()), "sha256": digest,
                      "rig_id": rig["rig_id"], "edits": edits}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
