#!/usr/bin/env python
"""Build the Blender porter extension with a pinned HD2 source contract."""

from __future__ import annotations

import argparse
import json
import pathlib
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
ADDON = ROOT / "blender" / "hd2_armature_porter"
FILES = ("blender_manifest.toml", "core.py", "__init__.py")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    source = pathlib.Path(args.source)
    document = json.loads(source.read_text(encoding="utf-8"))
    if document.get("schema") != "HD2SOURCE1":
        raise ValueError("--source is not an HD2SOURCE1 contract")
    destination = pathlib.Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in FILES:
            archive.write(ADDON / name, name)
        archive.write(source, "source.hd2source.json")
    print(destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
