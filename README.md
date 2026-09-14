# HD2 Armature Adjuster

This prototype locates and edits Helldivers 2 loader-converted inverse-bind tables from external patch profiles. Patch-specific bytes are no longer compiled into the ReShade add-on.

The validated mechanism is documented in [`CURRENT_STAGE_LOG_2026-09-14.md`](CURRENT_STAGE_LOG_2026-09-14.md). The external profile workflow and format are documented in [`PROFILE_PIPELINE.md`](PROFILE_PIPELINE.md), with live refactor evidence in [`DYNAMIC_PROFILE_VALIDATION_2026-09-14.md`](DYNAMIC_PROFILE_VALIDATION_2026-09-14.md).

The offline [`patch_profile_tool.py`](tools/patch_profile_tool.py) can inspect a
finished patch, create a verified translated copy, and generate its matching
runtime profile in one command. See [`PATCH_EDITING_TOOL.md`](PATCH_EDITING_TOOL.md).

## Current architecture

```text
finished mod-tree copy
        |
        v
patch_profile_tool.py pose-tree
        |
        +-- marked .patch_N   unweighted probe + repeated palette tail
        +-- .hd2profile       exact converted IB tables + patch identity
        +-- palette_markers   live-palette locator metadata
        |
        +-- shoulder_targets  scene graph + per-LOD RealIndices
                    |
                    +-- one uniform request for every arm descendant
                    |
                    v
HD2ArmatureProfiles beside the add-on
                    |
                    v
ReShade add-on: find IB -> locate live palette -> derive per-slot IB -> recover/reapply
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

For pose-aware shoulder adjustment, run the combined pipeline on a finished mod
tree. The output must not already exist and is a separate, installable copy:

```powershell
python tools/patch_profile_tool.py pose-tree `
  --root "C:\path\to\finished-mod" `
  --out-root "C:\path\to\finished-mod-pose-aware" `
  --left-translate 0.03 0 0 `
  --right-translate -0.03 0 0
```

This appends one unweighted probe slot followed by a variant-specific repeated
tail to every shoulder-bearing palette. GPU and stream companions are copied
byte-identically. It then creates the active profile union, `palette_markers.txt`,
and a full left/right arm-descendant target map. The marker lets the runtime
associate an uploaded animated palette with both its unit layout and the exact
inverse-bind revision that produced it.

For static diagnostic experiments, `profile-tree` can still be followed by:

```powershell
python tools/patch_profile_tool.py shoulder-targets `
  --root "build\converted-mod" `
  --profile-dir "build\converted-mod\HD2ArmatureProfiles"
```

This walks the unit scene-graph parents, maps the `l_shoulder` and `r_shoulder`
branches through every LOD's `RealIndices`, and qualifies every output slot with
the exact table's FNV-1a fingerprint. It rejects an exact runtime table if the
same slot has conflicting semantics in another LOD or replacement variant.

The standalone static command defaults to a tapered shape adjustment: shoulder entries receive
`0.03 m`, depth-one descendants `0.02 m`, depth-two descendants `0.01 m`, and
depth-three or deeper descendants are left pristine. This reaches zero at the
hand before the finger chains, avoiding the per-finger rotations observed when
the old uniform offset was propagated through the whole animated branch. Use
`--falloff-depth -1` reproduces the old full-branch static diagnostic. The
`pose-tree` command intentionally uses a uniform full branch: hierarchy chooses
the affected slots, while runtime pose math derives each slot's required IB
matrix instead of hardcoding a different displacement at every depth.
The two translation arguments are the only requested shoulder corrections;
change them to tune width without editing code or individual descendants.

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
5. For pose-aware shoulder adjustment, also copy `shoulder_targets.txt` and
   `palette_markers.txt` from the same generated directory.

If `active_profiles.txt` is absent, the add-on loads every `.hd2profile` in the directory. An explicit list is recommended because it prevents stale profiles from silently becoming active.

The add-on validates profile structure and checksums, then looks only for complete exact 48-byte-per-entry table matches. One `F8` press records a persistent requested-ON state. With marker metadata present, the first guarded write changes only the unweighted probe. The add-on passively scans game-mapped upload buffers for the repeated tail, verifies the probe code/revision, and reconstructs each arm slot's current native skin transform. One common inward correction is then conjugated through each slot's live pose to derive the next IB table. Scene recovery rebinds new exact table instances and re-establishes the probe automatically. The next `F8` press requests OFF and restores only tables whose complete bytes still equal this add-on's expected override. `F9` manually requests a full discovery pass without changing the F8 state.

`shoulder_targets.txt` uses one target per line:

```text
unit_id  table_fingerprint  slot  world_x  world_y  world_z
```

Blank lines and `#` comments are accepted. Legacy five-field unit-wide rows are
still accepted for controlled experiments, but generated table-qualified rows
are required for safe multi-LOD deployment.

## Automated validation

The runner verifies each profile's embedded patch provenance before deployment.
An exact archive-hash match is reported when present; alternate replacement
variants are allowed because the runtime still requires a complete exact table
match before any write. This is necessary when one profile union intentionally
covers several replacements of the same patch basename:

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

## Resource monitoring

The console and telemetry separate the recurring add-on work into IB scanner,
live-palette scanner, pose driver, per-frame maintenance, and rebind costs. Scanner CPU is reported as a percentage
of one logical core; maintenance and rebind percentages are their measured wall
time divided by the latest one-second sample interval. Whole-process CPU is
normalized across all logical processors and is included only for correlation.

Every run also creates
`%LOCALAPPDATA%\HD2ArmatureAdjuster\resource-monitor-<session>.csv`, with one row
per second. It records scan requests and coalescing, full versus priority runs,
bytes and CPU time spent scanning, live-palette bytes/hits and timing, pose
updates/skips and timing, table-maintenance reads, rebind timing, and the game's
working/private memory. Summarize the newest session with:

```powershell
python tools/summarize_resource_monitor.py
```

Pass a CSV path to summarize an older session. The monitor performs one process
resource query and one short CSV append per second; it does not add another
sampling thread or change scan/rebind scheduling.

## Scope

This prototype now implements external-profile discovery, persistent edit
intent, scene-change instance recovery, reversible writes, offline hierarchy
selection, patch marker generation, live-palette association, and pose-derived
full-arm IB updates. The pose-aware path has unit and offline tests but still
requires its first in-game validation; the previously validated tapered static
path remains the fallback when marker metadata is absent. A general naming
database and interactive editor remain future work.
