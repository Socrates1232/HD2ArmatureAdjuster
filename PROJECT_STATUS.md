# HD2 Armature Adjuster: Project Status and Findings

Updated: 2026-09-13

## Goal

Build an independent ReShade add-on and patching workflow that can identify Helldivers 2 skinning palettes, map their slots to useful armature concepts, and eventually apply reversible runtime adjustments. The reference PhysBone project is being studied only for the game-facing mechanism and data relationships.

The intended finished tool should not require a user-supplied armature, VRM file, or bone-name cheat sheet at runtime.

## Current stage

This repository is at the anonymous discovery and bounded-edit proof-of-concept stage. It contains:

- A loadable ReShade add-on that observes CPU-visible D3D12 buffers.
- A conservative scanner for transform-shaped arrays using 64-byte matrices, 48-byte packed affine matrices, and 32-byte dual quaternions.
- A standalone console that reports anonymous candidate slots and value changes.
- A live-test runner with process-state checks, exact game-window input gating, low-resolution screenshots, shutdown handling, and recovery-only mode.
- An opt-in edit probe that pulses one transform candidate, verifies readback, and restores the original bytes.

The add-on can expose changing transform-like values, but it cannot yet prove that a candidate is a character bone palette or associate a slot with vertices, a draw, a mesh, or a semantic bone name.

## What has been verified

- ReShade 6.5.1 (add-on API 17) loads the add-on, registers nine callbacks, exports metadata, and unloads it cleanly in the isolated load test.
- The add-on console appears in a real game run.
- During a manual run, displayed values changed as the character moved and turned. A visible change such as `0 -> -180` is evidence that live transform-like data is being observed; the values are raw matrix/translation deltas and should not yet be interpreted as named-bone Euler angles.
- Automated input can be sent only after the exact ReShade game window has focus. Comparing process IDs was insufficient because the add-on console and render window are both owned by `helldivers2.exe`.
- The runner can capture useful low-resolution checkpoints and distinguish a live process from an exited-but-still-present process object.

## Test record

| Test | Result | Interpretation |
| --- | --- | --- |
| Manual movement and turning | Console values changed | Confirms live data, not bone ownership or mapping |
| Run `20260913-180954` | Reached automation stage 7; sent `W`, then `W+D`, then `B`; captured gameplay; 1,179 draws in the last frame; up to three candidates; no candidate marked moving | Input emission is confirmed, but avatar displacement and slot correlation were not verified |
| Run `20260913-181357` | Three repeated intro-skip `W` attempts; 134 draws; one non-moving candidate; game crashed with Windows application errors `0xc000000d` and `0xc0000026` | Repeated injection was removed. The test is a crash correlation, not proof of its cause |
| Recovery-only test | Detected the exited-but-present game process, attempted exact-PID cleanup, refused unsafe thread termination, and blocked deployment | Recovery behavior worked as designed; Windows restart was the safe fallback while the stale process remained |
| Run `20260913-184754` | First edit-channel pass wrote and read back one 48-byte candidate, then restored it; target had no prior motion; shutdown briefly observed an exited process object which cleared immediately afterward | Memory channel verified, render ownership not established |
| Run `20260913-185144` | Motion-backed candidate 7, slot 5, 48-byte layout; 178,616 successful writes and immediate readbacks; three present-time readbacks; restoration and later engine overwrite observed; movement visibly displaced the character; clean shutdown | Control/feedback loop and writable allocation verified; captures show no unambiguous skeletal deformation |

The stale process later cleared naturally. At the time of this update, no `helldivers2.exe` process is present.

## What the reference project revealed

The PhysBone workflow does not reconstruct a complete armature purely from anonymous runtime buffers. Its offline patch stage parses and preserves identifying information that the runtime stage can later recognize:

- Scene-graph node indices and name hashes.
- Inverse-bind matrices.
- Per-material bone remap tables.
- Vertex bone indices and weights.
- Unit identity and palette layout metadata.

The patch also grows the palette and plants a distinctive structural fingerprint. Added slots refer to a known scene-graph node, while repeated inverse-bind entries form a recognizable run. This lets the runtime code locate its intended palette instead of treating every transform-shaped GPU buffer as a skeleton.

For a palette slot `k`, the observed relationship is:

```text
H_k = IB_k * W_node(k)
```

where `IB_k` is the inverse-bind matrix and `W_node(k)` is the node world transform. To request a desired skinning transform `S_k`, the reference mechanism derives an adjusted inverse bind:

```text
IB'_k = S_k * inverse(H_k) * IB_k
```

The inverse of an inverse-bind matrix also exposes the bone's bind-space transform. Its translation can therefore help reconstruct bone positions in unit vertex space.

This supports the current conclusion: reliable and stable semantic reconstruction needs an offline marker/metadata patch. A purely runtime scanner remains useful for discovery and validation, but it is not a dependable foundation for named bones or persistent slot identities.

## Proposed independent architecture

1. **Offline marker patch** — parse the game unit and mesh resources, insert a harmless recognizable palette fingerprint, and emit only the minimal mapping metadata needed by this tool. No VRM dependency is required.
2. **Runtime locator** — find the marked inverse-bind table and its current palette copies, while rejecting unrelated matrix arrays.
3. **Mapping layer** — connect unit instance, draw/LOD/material, palette slot, material bone remap, and influenced vertices. Scene-graph hashes and hierarchy can later provide semantic names.
4. **Runtime editor** — expose stable logical slots and apply reversible adjustments at the verified upstream inverse-bind or palette location.

A useful stable identity is expected to be hierarchical:

```text
unit instance -> draw/LOD/material -> palette binding -> slot -> influenced vertices
```

A raw GPU resource address and byte offset cannot serve as the public slot identity because upload-ring allocations can move between frames.

## Information available for armature reconstruction

- Scene-graph hierarchy, node indices, and name hashes from the unit resource.
- Inverse-bind transforms and derived bind-space positions.
- Material-section remap from mesh-local bone indices to palette slots.
- Per-vertex bone indices and weights.
- Draw, pipeline, descriptor, vertex-buffer, and index-buffer ownership observed at runtime.
- Temporal correlation across vanilla animation, and later controlled perturbation of one verified slot at a time.

A bone-hash dictionary can be used during development to validate recovered names, but should not be a required runtime dependency.

## Current limitations and open risks

- The scanner recognizes plausible numeric layouts, not skinning ownership. Intro and UI rendering can produce false positives, including repeated `[2, 0, 0]`-like values.
- There is no descriptor/draw ownership tracking, vertex influence mapping, or semantic naming yet.
- Anonymous buffer editing works, but a rendered character edit has not been proven. A moving transform-shaped candidate can still belong to camera, lighting, physics, or unrelated shader data.
- The game crash cause is not proven. Repeated input injection was correlated with one crash, and console teardown was a plausible contributor, so both areas were simplified. Neither should be treated as the confirmed root cause.
- Individual thread termination is intentionally not used for zombie recovery because it can corrupt process and driver state.

## Runner and safety policy

- Re-query `helldivers2.exe` before deployment, launch, input, and shutdown.
- Match both PID and process start time so a restarted process is never mistaken for the original test target.
- Send input only while the exact game render window has foreground focus.
- Use one delayed intro input attempt instead of repeated injection.
- Capture low-resolution load, start, ready, walk, stretch, edit-before, edit-active, and edit-restored checkpoints. High-resolution capture is reserved for cases where small visual details matter.
- Treat an exited-but-still-present process as a failed shutdown. Recovery-only mode attempts safe exact-PID cleanup, blocks DLL replacement while it is locked, and requests a Windows restart rather than terminating individual threads.

## Current deployment state

- Repository baseline before this document: `db49ad4` (`Add crash recovery checks and state captures`).
- No game process is currently present, and no automation request is pending.
- The current edit-test build was deployed and live-tested successfully. Run `20260913-185144` used SHA-256 `2988A682DB46693EEB49D7BB6049A7ED00BC80E0868B136A97F08F900C160190`.
- The tested process shut down cleanly, and a fresh process query found no remaining `helldivers2.exe`.

## Recommended next verification

The next development milestone is a minimal offline marker patch plus runtime recognition of that marker. The marker is needed to distinguish a character palette consumed by a draw from merely writable, animated transform-shaped data. Repeat the same bounded pulse against the marked palette; a visible before/active/restored difference will validate the final render path before semantic naming or full vertex mapping begins.
