# Custom Armature Adaptation — Stage 1

## Status

Stage 1 now has two connected deliverables:

- `HD2 Armature Adapter`, a Blender 4.2+ extension and headless exporter that turns a compatible edited rest armature into a deterministic `HD2RIG1` package.
- `HD2 Armature Profile Runtime 1.5`, a ReShade add-on that associates that package with exact active patch profiles, recovers current native bone motion from a live skinning palette, generates replacement inverse-bind values, and publishes/restores them with guarded revision ownership.

The original patches and `.vrm` files are read-only inputs. The workflow neither fingerprints nor rewrites them.

The offline implementation and the real 257-bone/27-table package have passed the repository test suite and the runtime's strict C++ package parser. Runtime 1.3 removed the synchronous scan/service path that froze the game, then exposed a false `SUBJECT_AMBIGUOUS` gate because repeated exact table instances were treated as conflicting subjects. Runtime 1.4 accepts every exact discovered instance for an associated profile. Runtime 1.5 also prevents undersized mapped buffers from consuming the capture throttle, while keeping matching and publication on the worker. It has not yet passed the replacement live gate. See [`CUSTOM_ARMATURE_LIVE_FAILURE_2026-09-14.md`](CUSTOM_ARMATURE_LIVE_FAILURE_2026-09-14.md).

## Supported Stage-1 edit class

Stage 1 supports a target armature with:

- the same logical bones and hierarchy as the exported source;
- an unchanged root;
- compatible rest bases;
- changed non-root rest translations, in metres.

Moving a shoulder root in Blender therefore moves its complete descendant arm hierarchy. There is no depth falloff and no per-finger hardcoding. The runtime evaluates the hierarchy and each frame's native transforms to obtain the required per-slot inverse-bind replacement.

Changing rest rotations/scales selects `full_native_pose`, which the package format can represent but this Stage-1 runtime deliberately rejects. Reparenting, adding/removing required bones, reweighting, IK solving, and patch mutation are outside Stage 1.

## Data flow

```text
finished mod patches + active .hd2profile files
                    |
                    | source-export (read only)
                    v
             *.hd2source.json
                    |
                    | Blender import / map / edit / export
                    v
               *.hd2rig.json
                    |
                    | validate + package
                    v
       bin/HD2ArmatureRigs/active_rig.txt
                    |
                    | exact unit/table/profile association
                    v
 mapped skin palette -> recover native W -> hierarchy evaluator
                    -> replacement IB plan -> guarded publisher
```

The source reference is the union of active armor pieces and LOD tables. Repeated patch variants are unioned by exact unit, entry count, and table SHA-256. Structural collisions receive distinct stable IDs, so a display name is never used as runtime identity.

## 1. Export a source reference

Generate normal runtime profiles first. Keep the selected filenames in `active_profiles.txt`, then run:

```powershell
python tools/rig_profile_tool.py source-export `
  --root "path\to\finished-mod" `
  --profiles "path\to\finished-mod\HD2ArmatureProfiles" `
  --out "work\source.hd2source.json" `
  --role chest=spine_2 `
  --role left_elbow=l_elbow `
  --role right_elbow=r_elbow `
  --role left_hand=l_hand `
  --role right_hand=r_hand
```

`l_shoulder` and `r_shoulder` are assigned automatically. Additional role names must come from known asset data; the exporter does not guess them.

The source export records:

- stable bone IDs, display names, parent IDs, and source local/global rest matrices;
- exact unit/table identities and per-slot stable-bone mappings;
- active profile filenames and SHA-256 values;
- a deterministic source definition ID.

Run the same export twice if reproducibility matters; byte-identical inputs produce byte-identical output.

## 2. Author a target in Blender

Install `hd2_armature_porter-1.4.0.zip` through Blender's **Preferences → Get Extensions → Install from Disk**. In the 3D View sidebar, open **HD2AA**. The release ZIP contains the source contract used to build it, so ordinary authoring does not require selecting that file.

For an armature already modified from an exported HD2 avatar rig:

1. Select the modified armature object and click **Use Selected Armature**.
2. In Edit Mode, select only the bone roots whose local rest transforms you intentionally changed, then click **Add Selected to Port**. The button reads Blender's EditBone selection and adds it to the existing marks; **Clear Port Marks** resets the set. Moving a complete shoulder branch normally requires marking its shoulder root, not every unchanged descendant.
3. Confirm that the panel says **Bundled source contract active**. A development copy without a bundled contract falls back to the external `*.hd2source.json` selector.
4. Optionally click **Check Automatic Mapping**. Only explicitly marked runtime bones are matched; every other source record remains unchanged. Non-skinning structural nodes such as `game_mesh` are excluded. When the union contract contains several structural variants of a marked bone, only variants compatible with the target bone's parent are selected.
5. Leave **Basis mode** at `Preserved` and **Capability** at `Automatic`.
6. Click **Validate Target** and require `linear_rest_translation` for the Stage-1 runtime. If validation selects `full_native_pose`, review the marked bones or deliberately use position normalization; do not deploy it to the Stage-1 runtime.
7. Choose an output ending in `.hd2rig.json`, then click **Validate & Export Port**.

Marked-name mapping expands a selected target bone to the parent-compatible stable source identities used by the active tables. Stable IDs, not names, are written into the package contract.

The alternate source-first workflow remains available: select the contract, click **Import Source**, click **Duplicate Imported Source**, and make rest-pose changes to that duplicate.

Apply the armature object's transforms before export. Shape changes belong in Edit Mode; Pose Mode animation is not exported as a rest-armature adaptation.

### Why the source contract remains necessary

The selected Blender armature contains bone names, hierarchy, and its current rest pose. It does not reliably contain the original unmodified rest pose, exact patch unit IDs, LOD-specific `RealIndices`, IB-table hashes, slot-to-bone mappings, or active profile hashes. Those values are necessary to calculate a delta instead of mistaking the target as the baseline, and to tell the runtime which exact table entries may be changed.

Consequently, removing the contract would make the export ambiguous or force the runtime back to hardcoded slot assumptions. Release builds bundle a versioned contract and use it automatically. The contract must be regenerated and the porter rebuilt when the selected patch/profile set changes; a development build without the bundled asset retains the external selector.

The headless equivalent is:

```powershell
blender file.blend --background --python tools/blender/export_hd2rig.py -- `
  --source "work\source.hd2source.json" `
  --target HD2AA_Target `
  --out "work\target.hd2rig.json"
```

For deterministic scripted translation of semantic roots, the same exporter core is available without Blender:

```powershell
python tools/translate_rig_roles.py `
  --source "work\source.hd2source.json" `
  --out "work\shoulders.hd2rig.json" `
  --role left_shoulder -0.06 0 0 `
  --role right_shoulder -0.06 0 0
```

That command is a validation/example path, not a runtime preset: the deltas are stored in the exported rig, never compiled into the add-on.

## 3. Validate and package

Validate against the exact active profile directory and source bytes:

```powershell
python tools/rig_profile_tool.py validate `
  --rig "work\target.hd2rig.json" `
  --source "work\source.hd2source.json" `
  --profiles "path\to\HD2ArmatureProfiles"
```

Assemble the two runtime files, activation record, and audit manifest:

```powershell
python tools/rig_profile_tool.py package `
  --rig "work\target.hd2rig.json" `
  --source "work\source.hd2source.json" `
  --profiles "path\to\HD2ArmatureProfiles" `
  --out-dir "release\HD2ArmatureRigs"
```

Packaging rejects a stale source hash, stale profile filename/hash dependency, unsupported capability, changed root, incompatible hierarchy, invalid slot, duplicate identity, or a non-finite/non-affine matrix.

The structural JSON Schema is [`schemas/hd2rig-v1.schema.json`](schemas/hd2rig-v1.schema.json). The Python validator and C++ runtime parser additionally enforce the semantic rules that JSON Schema cannot express.

The output is:

```text
HD2ArmatureRigs/
  active_rig.txt
  target.hd2rig.json
  source.hd2source.json
  package_manifest.json
```

## 4. Runtime installation

Only install with `helldivers2.exe` stopped:

```text
Helldivers 2/bin/
  HD2PaletteProbe.addon64
  HD2ArmatureProfiles/
    active_profiles.txt
    *.hd2profile
  HD2ArmatureRigs/
    active_rig.txt
    *.hd2rig.json
    *.hd2source.json
```

The runtime resolves `HD2ArmatureRigs` beside `HD2ArmatureProfiles`. `active_rig.txt` contains exactly two basenames: the rig on line 1 and its source reference on line 2.

At load, the runtime requires:

- the rig's pinned source-reference SHA-256 to match the selected source file;
- each table's unit ID, entry count, exact table SHA-256, profile filename, and profile file SHA-256 to match an active runtime profile;
- a supported `linear_rest_translation` capability report.

A valid custom rig takes ownership of F8. If no valid custom rig is configured, the old shoulder-target behavior remains the fallback.

## 5. Runtime behavior

F8 publishes only a persistent atomic desired-state revision and wakes a below-normal-priority worker:

- ON: discover all exact matching IB instances, locate a qualifying live palette, recover native bone transforms, build one plan per table layout, then publish it independently to every guarded instance.
- OFF: restore the exact pristine table only when the complete live bytes still equal this runtime's last verified revision.

F9 requests full IB rediscovery without changing the F8 desired state.

For a skin matrix `K`, current inverse bind `B`, and native world matrix `W`:

```text
W = K * inverse(B)
```

For a local rest-translation delta `delta_i`, the hierarchy evaluator computes:

```text
d_root = 0
d_i = d_parent + linear(W_parent) * delta_i
B_write_i = translate(inverse(linear(W_i)) * d_i) * B_mesh_i
```

This is why every descendant follows a moved clavicle while retaining its current animated orientation. The add-on does not contain shoulder slot numbers, bone names, or hand-authored per-level offsets.

The pose provider validates parent/child rest-length evidence before accepting a matrix-shaped region. Map callbacks first reject buffers smaller than one useful scan window, then consume the global capture throttle and copy a bounded 256 KiB snapshot with overlap into a single replaceable mailbox. The worker consumes that snapshot in 16 KiB steps with 8 KiB overlap, so it retains the coverage that located live palettes in runtime 1.2 without running matching on the presentation callback. Once a pose is found, a bounded neighborhood around that resource is preferred for continuity. Planning and publication also remain on the worker. All associated table layouts share one scan pass, and no new game buffer maps are issued by this scanner.

The publisher has one owner per table and a monotonic revision. Each changed 48-byte slot is written and read back. A partial failure rolls back to the last verified complete image. If a scene refills a retained table with exact pristine bytes, ownership rebases and the desired rig is reapplied. Unknown readable bytes are not overwritten.

Runtime status is explicit:

- `DISABLED`
- `WAITING_FOR_BIND_INSTANCE`
- `WAITING_FOR_LIVE_POSE`
- `LIVE_LAYOUT_UNVERIFIED`
- `SUBJECT_AMBIGUOUS`
- `APPLIED`
- `RESTORE_PENDING`
- `DIRTY_UNKNOWN`
- `CONFIGURATION_ERROR`

## 6. Monitoring

The console and `%LOCALAPPDATA%\HD2ArmatureAdjuster\telemetry.json` report custom mapped bytes, candidate palettes, qualified samples, built plans, publications, restores, scan calls, last/max scan time, requested/accepted intent revisions, worker busy state, and worker cycle time.

Each session also appends these custom fields to `resource-monitor-<session>.csv`. They separate the live-palette sweep from the existing process-memory IB discovery, maintenance, and rebind measurements.

## 7. Verification completed

The current offline gate covers:

- deterministic source and rig export;
- strict JSON/package parsing and exact profile association;
- column/row convention adapters and affine inverses;
- exact hierarchy propagation under animated rotations;
- T48 encoding and a 500-cycle no-drift test;
- mapped-palette recovery and stranger rejection;
- guarded, failure-atomic publishing and restoration;
- scene refill/reapply at a retained address;
- add-on loading and ReShade callback registration;
- the pre-existing profile, shoulder, scene-sidecar, monitoring, and lifecycle tests.

The test suite currently reports 18/18 passing, including the atomic intent-mailbox and non-blocking presentation-path regressions. The real validation package contains 257 logical bones, 27 exact tables, and 1,625 slot mappings, and it loads successfully through the runtime's C++ `HD2RIG1` parser.

The remaining gate is a controlled in-game run of runtime 1.5. Until a run remains responsive after F8, reaches `APPLIED`, visibly follows animation, restores on F8, and survives a scene transition, the runtime should be described as implemented and offline-verified—not live-validated.

## Implementation record

- Working branch: `custom-armature-stage1`
- Pre-Stage-1 source head: `83d04e805ae170de7f2e40c2b93ed109819cd9d3`
- Working-baseline merge base: `361928ef11797570bfc8fdf2bba3d6eef4e0917b`
- Retarget-core commit: `8f65553`
- Porter-pipeline commit: `b0dd16d`
- Runtime commit: `a220a55`
- Completed workflow commit: `41bafe0`
- Installed legacy add-on observed before deployment: SHA-256 `dc6a72aa8b8e9770e3e7ae62f25385e2cc2e140b9091882bea282d3488a639f4`, 432,640 bytes
- Real source export: SHA-256 `80eff04df8d8a5690a1917ef5c0a61eb2972351035a2ff5111f109ac88b83372`; a second independent export was byte-identical
- Real example rig: SHA-256 `5284c789b0343329ecbc5125d101e2551c6fa113016ee6c442c214077d5cdc23`

The supplied NumPy oracle passed its 20 tests before implementation. Blender itself was not present on this machine, so the extension source was syntax-checked and its archive layout verified, but its UI operators still require a Blender smoke test.
