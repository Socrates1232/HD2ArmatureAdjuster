# HD2 Armature Adjuster

This prototype locates and edits Helldivers 2 loader-converted inverse-bind tables from external patch profiles. Patch-specific bytes are no longer compiled into the ReShade add-on.

The validated mechanism is documented in [`CURRENT_STAGE_LOG_2026-09-14.md`](CURRENT_STAGE_LOG_2026-09-14.md). The external profile workflow and format are documented in [`PROFILE_PIPELINE.md`](PROFILE_PIPELINE.md), with live refactor evidence in [`DYNAMIC_PROFILE_VALIDATION_2026-09-14.md`](DYNAMIC_PROFILE_VALIDATION_2026-09-14.md).

The offline [`patch_profile_tool.py`](tools/patch_profile_tool.py) can inspect a
finished patch, create a verified translated copy, and generate its matching
runtime profile in one command. See [`PATCH_EDITING_TOOL.md`](PATCH_EDITING_TOOL.md).

## Current architecture

```text
finished .patch_N
        |
        v
extract_runtime_profile.py
        |
        +-- .hd2profile       exact converted IB tables + patch identity
        +-- .hd2profile.json  reviewable manifest
                    |
                    v
HD2ArmatureProfiles beside the add-on
                    |
                    v
ReShade add-on: exact locate -> guarded slot edit -> readback -> restore
```

Each profile represents one finished main patch bundle. It may contain several unit IDs and LOD tables. Identical tables shared by multiple LODs are stored once with an LOD mask. Multiple active profile files compose into one runtime registry; records with the same unit ID, entry count, and exact table bytes are merged during loading.

The current example profile contains B-01 unit `fa269172bd08695b`: one 88-entry table shared by LODs 0-3 and separate 2-entry and 4-entry reduced LOD tables.

## Generate a profile

Run this after a patch has reached its final form:

```powershell
python tools/extract_runtime_profile.py `
  --patch "path\to\0123456789abcdef.patch_0" `
  --out "profiles\runtime\0123456789abcdef.patch_0.hd2profile"
```

By default every parseable unit resource in the patch is extracted. To restrict the output, repeat `--unit`:

```powershell
python tools/extract_runtime_profile.py `
  --patch "path\to\0123456789abcdef.patch_0" `
  --unit fa269172bd08695b `
  --out "profiles\runtime\b01-chest.hd2profile"
```

The command also writes `<output>.json`. Treat a nonzero exit as a patch-pipeline failure. Do not modify the patch after extraction; its SHA-256 is embedded in the profile.

To apply a validated inverse-bind translation and profile the edited copy in one
step:

```powershell
python tools/patch_profile_tool.py translate `
  --patch "path\to\source\0123456789abcdef.patch_0" `
  --out-patch "build\edited\0123456789abcdef.patch_0" `
  --unit fa269172bd08695b `
  --slot 7 `
  --translate 0.0 0.15 0.0
```

## Build

The build uses the official ReShade 6.5.1 headers at add-on API 17. The pinned SDK revision is `f1332dfe8fb1c61a726d53af069cc2a2fcacae7f`.

```powershell
git clone --depth 1 --branch v6.5.1 --filter=blob:none --sparse https://github.com/crosire/reshade.git deps/reshade
git -C deps/reshade sparse-checkout set --no-cone /include/ /LICENSE.md
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The Visual Studio build produces `build/bin/Release/HD2PaletteProbe.addon64`.

## Install

With the game stopped:

1. Copy `HD2PaletteProbe.addon64` into `Helldivers 2\bin`, beside ReShade's `dxgi.dll`.
2. Create `Helldivers 2\bin\HD2ArmatureProfiles`.
3. Copy the desired `.hd2profile` files into that directory.
4. Put their filenames, one per line, in `active_profiles.txt` in the same directory.
5. For the shoulder test, copy a target map as `shoulder_targets.txt` in that directory.

If `active_profiles.txt` is absent, the add-on loads every `.hd2profile` in the directory. An explicit list is recommended because it prevents stale profiles from silently becoming active.

The add-on validates profile structure and checksums, then looks only for complete exact 48-byte-per-entry table matches. `F8` starts a fresh shoulder-only scan and toggles the configured narrowing, restoring the exact source bytes on the next press. If an active table becomes unreadable or changes unexpectedly for three consecutive frames, the add-on safely abandons its old addresses and automatically reacquires the shoulder tables. `F9` restores an active shoulder edit and requests a general rescan. The standalone console reports loaded files, tables, errors, unit IDs, LOD masks, entry counts, matched addresses, shoulder state, and reacquisition count.

`shoulder_targets.txt` uses one target per line:

```text
unit_id  slot  world_x  world_y  world_z
```

Blank lines and `#` comments are accepted. The supplied
`profiles/lacrima_dump_shoulder_targets.txt` moves each mapped left shoulder
`+0.08 m` on X and each right shoulder `-0.08 m` on X. These unit/slot mappings
come from the offline scene-graph and palette census of the Lacrima patch set.

## Automated validation

The runner validates that each profile's embedded main-patch basename and SHA-256 match the installed patch before deploying anything:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 `
  -GameRoot "F:\Steam\steamapps\common\Helldivers 2" `
  -PreflightOnly
```

Run a read-only exact-match test:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 `
  -ObserveSeconds 90 -AutomateInput -SteamCapture -ShutdownAfterTest
```

Run an explicit reversible edit, expressed as a world-space translation in metres:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 `
  -ObserveSeconds 90 -AutomateInput -EditTest `
  -EditUnit fa269172bd08695b -EditSlot 7 -EditX -1.5 `
  -SteamCapture -ShutdownAfterTest
```

An edit is attempted only after a full profile-table match. Every target must still equal the profiled source bytes immediately before writing. The add-on verifies its write, maintains it if the engine refills the exact source value, and restores only values that still equal its injected bytes. Unknown third-party or engine states are not overwritten.

Telemetry is written to `%LOCALAPPDATA%\HD2ArmatureAdjuster\telemetry.json`; test artifacts go to `test-results/<timestamp>`.

## Scope

This proves external-profile-driven inverse-bind discovery and reversible slot control. The F8 test adds one external, patch-derived semantic mapping for left and right shoulders. It does not yet provide a general naming database, hierarchy-aware edits, or an interactive editor.
