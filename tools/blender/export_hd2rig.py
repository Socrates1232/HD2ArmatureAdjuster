"""Headless Blender wrapper for the HD2 Armature Adapter exporter.

Example:
  blender file.blend --background --python export_hd2rig.py -- \
    --source source.hd2source.json --target HD2AA_Target --out target.hd2rig.json
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import bpy


ADDON_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ADDON_ROOT))
from hd2_armature_porter import collect_target  # noqa: E402
from hd2_armature_porter.core import build_rig, write_rig  # noqa: E402


def main() -> int:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--basis-mode", default="preserved",
                        choices=("preserved", "position_normalized", "exact_full"))
    parser.add_argument("--capability", default="auto",
                        choices=("auto", "linear_rest_translation", "full_native_pose"))
    parser.add_argument("--notes", default="")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(arguments)
    target = bpy.data.objects.get(args.target)
    local, parents = collect_target(target)
    rig = build_rig(args.source, local, parents, args.basis_mode, args.capability, args.notes)
    digest = write_rig(args.out, rig, args.force)
    print(f"HD2AA exported {len(rig['bones'])} bones, {len(rig['tables'])} tables, sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
