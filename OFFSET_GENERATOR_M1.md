# Pose-correct IB replacement producer — M1

This branch is rooted at the known working runtime revision `361928e`. Milestone
M1 adds offline semantics and pure replacement generation only. It does not
change `src/addon.cpp`, F8/F9 handling, discovery, instance ownership,
maintenance, restoration, the `HD2IBP1` format, or the legacy shoulder-target
parser. It also does not touch an installed game.

## Producer contract

`src/ib_replacement_generator.hpp` accepts:

- an immutable baseline converted inverse-bind table;
- one verified observed skinning 3x3 linear block per requested slot;
- a desired displacement in the palette skin-output frame `O`.

With row vectors it calculates:

```text
K_i = B_i * W_i
u_i = d_side * inverse(A(K_i))
B_i' = T(u_i) * B_i
```

The returned `table_plan` is a complete candidate byte image. The producer has
no process pointer and performs no write. Every entry is generated from pristine
`B_i`; earlier candidate bytes are never used as input. It copies the nine t48
linear floats byte-for-byte and changes only packed translation indices 3, 7,
and 11. Exact zero displacement bypasses inversion and returns exact baseline
bytes. A bad request rejects the whole plan and returns the pristine table.

The general affine inverse and `B * W_desired * inverse(W_native)` encoder are
pure preparation for a later milestone. They are not connected to the runtime.

## Semantic sidecars

`tools/patch_profile_tool.py rig-sidecars` writes one versioned
`*.hd2rig.json` per patch/unit plus `rig_sidecars_manifest.json`. It records:

- source patch triplet and unit hashes;
- per-LOD file64 and converted-t48 table identities;
- raw and interpreted parent records with cycle/range validation;
- raw TransformData plus its actual rotation/position/scale fields;
- stored rest matrices and a reported parent-equation check;
- per-LOD slot-to-scene-node mappings;
- explicit role resolution status;
- complete left/right shoulder descendants without depth taper;
- explicit unknown live-layout, observer, and output-frame relationships.

Only `l_shoulder` and `r_shoulder` are default names. Chest, elbow, and hand
names are deliberately explicit `--role` inputs until a specific asset source
establishes them. Missing or unconfigured roles remain visible in the JSON.

The old `shoulder-targets` command retains its existing tapered behavior and
format. Sidecars remain separate from `HD2IBP1` so this milestone cannot reroute
the working runtime.

## Gate

The M1 gate is automated by `inverse_bind_replacement_generator` and
`semantic_rig_sidecar_pipeline`, alongside every pre-existing test. The tests
cover t48 packing, rotated branches, translation-only feedback, randomized
nonuniform scale/shear, branch and mixed-weight behavior, 500-update drift,
linear contamination, stale samples, shared-table limits, exact zero, bad
bases, failure-atomic table planning, semantic validation, full descendants,
and source patch immutability.

Live pose acquisition and runtime integration remain M2 and M3. The producer is
not evidence that a live 48-byte packet has the right layout, ownership, slot
mapping, or timing.
