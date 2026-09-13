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
- An opt-in edit sweep that applies conspicuous translations to all slots in each candidate, verifies immediate and present-time readback, captures the result, and restores the original bytes when the mapped range remains accessible.

The add-on can expose changing transform-like values, but it cannot yet prove that a candidate is a character bone palette or associate a slot with vertices, a draw, a mesh, or a semantic bone name.

## What has been verified

- ReShade 6.5.1 (add-on API 17) loads the add-on, registers nine callbacks, exports metadata, and unloads it cleanly in the isolated load test.
- The add-on console appears in a real game run.
- During a manual run, displayed values changed as the character moved and turned. A visible change such as `0 -> -180` is evidence that live transform-like data is being observed; the values are raw matrix/translation deltas and should not yet be interpreted as named-bone Euler angles.
- Automated input can be sent only after the exact ReShade game window has focus. Comparing process IDs was insufficient because the add-on console and render window are both owned by `helldivers2.exe`.
- The runner can capture useful low-resolution checkpoints and distinguish a live process from an exited-but-still-present process object.
- The original scanner's attempt to map CPU-visible D3D12 buffers itself was removed. Discovery now reads only ranges currently mapped by the game.
- The passive scanner completed a full automated run, including walking, turning, stretch input, a full edit sweep, evidence capture, and clean process shutdown without another device-hung crash.

## Test record

| Test | Result | Interpretation |
| --- | --- | --- |
| Manual movement and turning | Console values changed | Confirms live data, not bone ownership or mapping |
| Run `20260913-180954` | Reached automation stage 7; sent `W`, then `W+D`, then `B`; captured gameplay; 1,179 draws in the last frame; up to three candidates; no candidate marked moving | Input emission is confirmed, but avatar displacement and slot correlation were not verified |
| Run `20260913-181357` | Three repeated intro-skip `W` attempts; 134 draws; one non-moving candidate; game crashed with Windows application errors `0xc000000d` and `0xc0000026` | Repeated injection was removed. The test is a crash correlation, not proof of its cause |
| Recovery-only test | Detected the exited-but-present game process, attempted exact-PID cleanup, refused unsafe thread termination, and blocked deployment | Recovery behavior worked as designed; Windows restart was the safe fallback while the stale process remained |
| Run `20260913-184754` | First edit-channel pass wrote and read back one 48-byte candidate, then restored it; target had no prior motion; shutdown briefly observed an exited process object which cleared immediately afterward | Memory channel verified, render ownership not established |
| Run `20260913-185144` | Motion-backed candidate 7, slot 5, 48-byte layout; 178,616 successful writes and immediate readbacks; three present-time readbacks; restoration and later engine overwrite observed; movement visibly displaced the character; clean shutdown | Control/feedback loop and writable allocation verified; captures show no unambiguous skeletal deformation |
| Run `20260913-192430` | Callback-timed drastic sweep completed 47/47 candidates and 367,495 verified writes; every slot received alternating XYZ translations of `+/-4`, `+/-8`, and `+/-12`; 14 candidate captures; clean shutdown | No capture showed credible skeletal deformation; writes made from the graphics callback still did not establish render ownership |
| Run `20260913-204351` | Passive mapped-range scanner; movement and stretch completed; 41/41 hammer-thread rounds; 7,372 successful writes and immediate readbacks; 30 present-time readbacks; 35 captures; final restoration and clean shutdown; one transient unmap produced edit error 2 | Stability regression passed with no crash. The memory channel is live, but the run is conservatively classified `failed-edit` because one target became inaccessible before an exact restore. No visible character deformation was found |

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
- Anonymous buffer editing works, but a rendered character edit has not been proven. Even a drastic all-slot sweep produced no credible deformation. A moving transform-shaped candidate can still belong to camera, lighting, physics, or unrelated shader data, or be overwritten before the consuming GPU work.
- Windows recorded `DXGI_ERROR_DEVICE_HUNG` in D3DDRED2 data for the recent crashes, followed by an `ntdll` BEX64 error. The add-on's self-mapping of D3D12 buffers was the highest-risk operation and has been removed. The first full passive-scanner run was stable, which strongly implicates that path but does not prove a single root cause.
- Passive observation can lose a candidate when the game unmaps or destroys its resource. The edit test treats loss before exact restoration as a failure instead of hiding it as a successful channel verification.
- Individual thread termination is intentionally not used for zombie recovery because it can corrupt process and driver state.

## Runner and safety policy

- Re-query `helldivers2.exe` before deployment, launch, input, and shutdown.
- Match both PID and process start time so a restarted process is never mistaken for the original test target.
- Send input only while the exact game render window has foreground focus.
- Use one delayed intro input attempt instead of repeated injection.
- Capture low-resolution load, start, ready, walk, stretch, edit-before, edit-active, and edit-restored checkpoints. High-resolution capture is reserved for cases where small visual details matter.
- Treat an exited-but-still-present process as a failed shutdown. Recovery-only mode attempts safe exact-PID cleanup, blocks DLL replacement while it is locked, and requests a Windows restart rather than terminating individual threads.

## Current deployment state

- Last published repository commit before this update: `c79024b` (`Add bounded runtime edit validation`).
- No game process is currently present, and no automation request is pending.
- The passive edit-test build used SHA-256 `74B077E9B52CAB0B13F45445F4044220AB07C46058CDE6CF10D492509FACFA1E` in run `20260913-204351`.
- The tested process shut down cleanly. A fresh process query found no remaining `helldivers2.exe`.

## Recommended next verification

Before building the offline fingerprint and semantic mapping, the next verification should establish one visible deformation. The current anonymous scanner is insufficient. The smallest useful next step is draw-bound resource correlation: record the descriptors and buffer copies associated with character draw calls, narrow the candidate set to data actually consumed by those draws, and repeat the same before/active/restored pulse. Only after that positive render-path result should the offline marker encode a stable identity for the proven palette.
