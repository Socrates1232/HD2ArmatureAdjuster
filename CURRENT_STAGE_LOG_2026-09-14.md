# HD2 Armature Adjuster: Current Stage Technical Log

Snapshot date: 2026-09-14

Mechanism-validation commit: `1a9b1cc`

> **Architecture update (later on 2026-09-14):** the patch-specific A/B tables described in the historical sections below are no longer compiled into the add-on. The current implementation extracts immutable `HD2IBP1` packages from each finished patch and loads any selected set at runtime. See [`PROFILE_PIPELINE.md`](PROFILE_PIPELINE.md) and [`DYNAMIC_PROFILE_VALIDATION_2026-09-14.md`](DYNAMIC_PROFILE_VALIDATION_2026-09-14.md). The external path reproduced four exact hits, four guarded writes, 588 present-time readbacks, and exact restoration of all four targets. The old A/B run remains the evidence that established the underlying mechanism; its hardcoded packaging is superseded.

Game: Helldivers 2

Runtime: ReShade 6.5.1 with full add-on support, add-on API 17

Validated unit: B-01 Lean chest, unit ID `fa269172bd08695b`

## Purpose and authority of this document

This is the authoritative current-stage description of the prototype. It consolidates the mechanism, implementation, offline and runtime workflow, evidence, failed approaches, safety rules, current deployment state, and next work.

The other reports remain useful for provenance:

- [`TEST_REPORT_2026-09-14.md`](TEST_REPORT_2026-09-14.md) is the complete ledger for the first 69 experimental runs. Its negative runtime verdict was correct at that point but has since been superseded.
- [`REVIEW_RESPONSE_2026-09-14.md`](REVIEW_RESPONSE_2026-09-14.md) records the architectural correction from upload-ring targeting to loader-converted inverse-bind targeting.
- [`LIVE_VALIDATION_2026-09-14.md`](LIVE_VALIDATION_2026-09-14.md) records the three controlled runs that established the corrected mechanism.
- [`PROJECT_STATUS.md`](PROJECT_STATUS.md) describes an older anonymous-scanner stage and must not be used alone to guide new work.

## Executive status

The mechanism-validation milestone has passed for one existing weighted slot in one B-01 chest unit.

The add-on can:

1. load through ReShade in the game's D3D12 process;
2. locate exact 88-entry, 48-byte-per-entry loader-converted inverse-bind tables in writable private process memory;
3. classify each complete table as known profile A or profile B;
4. replace one known slot with the exact bytes from the opposite profile;
5. verify the write immediately and on later present callbacks;
6. restore the exact original bytes; and
7. produce a controlled Steam screenshot sequence showing the rendered deformation disappear and return.

The decisive test used a deliberately exaggerated 1.5 m displacement of slot 7. Four exact profile-B tables were found. All four were switched to profile A for one second and then restored to B. The large left-shoulder-weighted displacement disappeared during A and returned during restored B. The camera and idle pose were kept fixed.

This proves live render control at the converted inverse-bind layer. It does not yet provide a general armature editor, automatic semantic bone names, per-vertex ownership display, arbitrary interactive transforms, or support for every armor component.

The prototype is also no longer dependency-free in the strict discovery sense. It requires no VRM, armature file, or cheat sheet at runtime, but its locator depends on offline profiles extracted from finished patches. Those profiles are external runtime data, not compiled constants. General semantic naming and vertex/remap mapping remain future work.

## Evidence level reached

The project uses the following evidence ladder:

| Level | Evidence | Meaning |
| --- | --- | --- |
| 0 | Add-on registration and fresh heartbeat | The DLL loaded and callbacks execute |
| 1 | Matrix-like values move with gameplay | Live transform-like memory was observed |
| 2 | Write and immediate readback | A CPU-visible location is writable |
| 3 | Later engine overwrite or refill | The allocation is reused or refreshed |
| 4 | Controlled before/active/restored visual change | The changed bytes affect the rendered frame |
| 5 | Complete unit/draw/material/slot/vertex ownership | A stable public editing identity is reconstructed |

The corrected converted-table experiment reached level 4. The unit and slot are known from offline structure, but full draw/material/vertex ownership and general semantic reconstruction are not complete, so level 5 is not claimed.

## Correct mechanism model

### The three data layers

Earlier experiments conflated three related but non-interchangeable objects:

| Layer | Format | Lifetime and role | Current use |
| --- | --- | --- | --- |
| Patch inverse binds | 64-byte affine matrices, called `file64` here | Static unit-resource setup data | Offline parsing and profile construction |
| Loader-converted inverse binds | 48-byte affine transforms, called `t48` | Runtime private tables derived when a skinned unit loads | Verified control point |
| Animated skinning/upload data | Often 48-byte transform-shaped copies | Downstream per-frame or upload-ring output | Not required by the successful edit path |

An offline edit works because the game converts the already-edited `file64` data when it loads the unit. Editing an arbitrary surviving `file64`-like allocation after conversion does not necessarily affect the instantiated character.

Likewise, a moving 48-byte matrix array is not automatically an inverse-bind table. It may be downstream animation output, camera data, lighting data, physics data, a stale upload-ring copy, or an unrelated transform array.

### Skinning relationship

For palette slot `k`, the reference mechanism implies the row-vector relationship:

```text
H_k = IB_k * W_node(k)
```

where:

- `IB_k` is the slot's inverse-bind transform;
- `W_node(k)` is the current animated world transform of the carrier scene-graph node; and
- `H_k` is the resulting skinning transform supplied downstream.

If a desired skinning transform is `S_k`, an adjusted inverse bind can be derived as:

```text
IB'_k = S_k * inverse(H_k) * IB_k
```

The game can continue running its normal animation. Changing `IB_k` changes the bind-space relationship through which that animation reaches weighted vertices. This is why bind data can act as a runtime adjustment layer even though inverse binds are normally static setup data.

The current validation does not yet calculate arbitrary `IB'_k` continuously. It switches one slot between two exact prevalidated inverse-bind states. That smaller operation was chosen to isolate the mechanism and make restoration unambiguous.

### `file64` to `t48` conversion

For a 16-float `file64` matrix `m[0..15]`, the verified packed representation is:

```text
t48 = [
    m0, m4, m8,  m12,
    m1, m5, m9,  m13,
    m2, m6, m10, m14
]
```

The packed translation components are therefore `t48[3]`, `t48[7]`, and `t48[11]`.

An older generic 48-byte helper treated indices 9, 10, and 11 as translation. That was wrong for the converted inverse-bind representation and was one reason generic runtime edits could not establish the intended mechanism.

The conversion and inverse relationship are implemented in [`src/ib_layout.hpp`](src/ib_layout.hpp) and covered by [`tests/ib_layout_test.cpp`](tests/ib_layout_test.cpp).

## Validated B-01 control profile

### Target identity

| Field | Value |
| --- | --- |
| Patch archive | `9ba626afa44a3aa3.patch_0` |
| Unit ID | `fa269172bd08695b` |
| Offline unit description | B-01 Lean chest |
| Palette sizes by LOD | `88, 88, 88, 88, 2, 4` |
| Controlled slot | 7 |
| Offline census label | `l_shoulder` |
| Profile table | LOD 0, 88 entries, 4,224 bytes after conversion |
| A state | Original inverse bind |
| B state | 1.5 m displacement along world-space negative X |
| Modified LODs | 0, 1, 2, 3 |
| Palette growth | None |
| Vertex reweighting | None |

The `l_shoulder` name is not inferred from runtime motion. It comes from the existing offline unit census. The runtime add-on only knows unit ID, slot index, and exact A/B bytes.

LODs 4 and 5 contain only two and four entries respectively, so slot 7 does not exist in them and they remain unchanged.

### Exact hashes

Profile A, the original patch triplet:

```text
main   fb181c4f3d4beb6500eef50a0a94977b20a7388252dd4925265eb0d504f23d23
gpu    a4cb635c27ff4ca0056df2a5ab9caaef7cabd3aafc698038b01ce962cb10fd1f
stream e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

Initial profile B, 0.15 m displacement:

```text
main   fabd2c95511310686b0ee4e4531dc733d18106b505b34af3a19cb1c54913a16f
gpu    a4cb635c27ff4ca0056df2a5ab9caaef7cabd3aafc698038b01ce962cb10fd1f
stream e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

Final exaggerated profile B, 1.5 m displacement:

```text
main   7d8d0fa1c3203ffdc993aeaac925cf148c04c4f41dff0ab10f10b1994591110e
gpu    a4cb635c27ff4ca0056df2a5ab9caaef7cabd3aafc698038b01ce962cb10fd1f
stream e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

The main file remains 50,184 bytes. The GPU resource remains 5,173,272 bytes and the stream is empty. Forty-eight individual bytes differ between A and exaggerated B, all inside slot 7's three-float translation rows in LODs 0–3. The surrounding matrices, table counts, unit placement, GPU resource, stream resource, remaps, and vertex weights are unchanged.

### How the exaggerated control was generated

The already validated 0.15 m A/B delta was scaled by ten with [`tools/scale_slot_control.py`](tools/scale_slot_control.py). The script:

1. parses A and the validated seed B;
2. requires equal bundle size and identical unit placement;
3. requires identical inverse-bind table layouts;
4. verifies that the seed differs only in `file64` translation indices 12, 13, and 14 for the selected slot;
5. computes `A + factor * (B - A)` for those translation floats;
6. writes all other bytes from A unchanged;
7. copies the original GPU and stream resources unchanged; and
8. rejects output whose changed bytes escape the allowed translation rows.

[`tools/build_ib_profile.py`](tools/build_ib_profile.py) independently reparses the final A/B pair and enforces a second set of gates:

- same main-bundle length;
- same unit offset and payload length;
- same BoneInfo LOD structure, counts, and offsets;
- no changes outside the selected slot's translation rows;
- non-identical A and B;
- byte-identical GPU and stream resources; and
- selected slot present in the chosen profile LOD.

At the original mechanism-validation commit it emitted:

- [`profiles/b01_slot7_ab.json`](profiles/b01_slot7_ab.json), containing hashes, sizes, LOD records, and exact slot bytes; and
- `src/ib_profile_data.hpp`, containing the exact 88-entry A and B `t48` tables compiled into the add-on. This generated header has since been removed.

The profile JSON stores patch basenames, not local machine paths. The generated binary patch workspaces remain local and are not committed.

## Runtime add-on architecture

### Load and lifecycle

The output DLL is `HD2PaletteProbe.addon64`. ReShade loads it into `helldivers2.exe`. The add-on requests API version 17 and only initializes its working state for a D3D12 device.

Registered callbacks include device/resource lifecycle, buffer map/unmap, indexed draw, present, and ReShade present events. The standalone console opens when the D3D12 device initializes.

The current compiled base mode is `converted_ib_scan`. An explicit automation request with edit enabled changes the reported and active mode to `converted_ib_edit`.

Legacy anonymous-buffer, marker-tail, residency-probe, and animated-upload code still exists in [`src/addon.cpp`](src/addon.cpp) for historical diagnosis. It is not the successful path:

- `k_enable_anonymous_scan` is `false`;
- the compiled experiment mode is `converted_ib_scan`;
- converted edit enforcement runs from `on_present`; and
- the old draw-indexed upload write path is guarded off whenever converted editing is requested.

This remaining legacy code is technical debt and should be removed when the verified path is extracted into a clean editor.

### Standalone console

The current console refreshes every 250 ms and reports:

- experiment mode, frame number, and indexed draws in the latest frame;
- tracked and currently mapped D3D12 buffers;
- converted-table hunt phase, pass count, bytes scanned, exact hits, partial candidates, and best partial entry count;
- edit armed/active/completed state;
- selected versus written targets, write attempts and successes, refills, and restoration state; and
- the legacy candidate page when anonymous candidates exist.

Because anonymous scanning is disabled, the legacy per-slot candidate page is normally empty and the console says that the mapped-buffer scan is disabled. `F9` is the relevant current control. `F6`, `F7`, and `F8` belong to the legacy candidate browser and should be removed or reassigned in the stripped interface.

The console header still prints the sentence “Read-only exact search” even during an explicitly requested edit pulse. The active mode and edit counters are correct, but that sentence is a known cosmetic inconsistency. The console is therefore diagnostic output, not yet the requested clean slot-editing interface.

### Exact converted-table scanner

The scanner runs on a worker thread so the render callback does not perform the multi-gigabyte process scan.

Its current procedure is:

1. Obtain the process application's valid address range.
2. Enumerate it with `VirtualQuery`.
3. Accept only committed `MEM_PRIVATE` regions whose protection is `PAGE_READWRITE` or `PAGE_WRITECOPY`.
4. Reject guarded, inaccessible, and write-combined pages.
5. Exclude the add-on image, scanner scratch buffer, and memory ranges already known as mapped graphics buffers. This prevents compiled profile self-matches and keeps downstream upload buffers out of the converted-table population.
6. Read accepted regions in 4 MiB chunks with enough overlap for one complete table.
7. Search on four-byte boundaries.
8. Use the 48-byte bytes of slot 7 as a fast A/B anchor.
9. Accept a target only if all 88 entries, 4,224 bytes, match complete profile A or complete profile B.
10. Record anchor matches that fail the full-table comparison as partial diagnostics, including the best number of matching entries.

The scan is bounded by 40 passes and a 30-second hunt deadline, with 500 ms between unsuccessful passes. Exact hits are capped at 32.

`F9` clears the current hits and requests a new hunt after the previous worker has finished.

Exact whole-table matching is intentionally conservative. It greatly reduced false ownership claims, but it is B-01/profile-specific and expensive. A game update, another armor unit, table reordering, canonicalization, per-instance changes, or another allocation protection can make it miss a valid table.

### Reversible edit loop

Converted edit mode never applies a guessed arithmetic edit to an anonymous matrix. It switches exact known bytes:

1. Require the scanner to be in its found state.
2. Snapshot the complete list of exact table hits.
3. For every hit, calculate `table address + slot * 48`.
4. Select the source bytes according to the hit's classified variant.
5. Select the exact slot bytes from the opposite variant as the destination.
6. Read the current 48 bytes and require them to equal the expected source exactly. A stale or changed location is skipped.
7. Write exactly 48 bytes with `WriteProcessMemory`.
8. Read the bytes back and require an exact destination match. If verification fails, immediately attempt to put the source bytes back.
9. Keep successfully written targets for the active interval.

During the one-second active interval, every present callback checks each target:

- If the destination bytes are still present, increment `edit_present_readbacks`.
- If the engine restored the exact source bytes, increment the refill counter and reapply the destination with another immediate verification.
- If the memory contains neither exact source nor exact destination, treat it as stale or foreign and do not overwrite it.

At the end of the pulse, restoration is similarly exact:

- A target already containing its original bytes counts as restored.
- A target containing the injected bytes is written back to the original bytes and verified.
- A target containing any third state is not overwritten and causes restoration failure.
- The aggregate restore succeeds only if every written target is restored and at least one target was written.

This check-before-write and check-before-restore policy prevents the add-on from blindly modifying an address after its ownership or lifetime changes.

### Telemetry

The add-on writes an atomic JSON heartbeat approximately once per second to:

```text
%LOCALAPPDATA%\HD2ArmatureAdjuster\telemetry.json
```

It writes a temporary file and replaces the previous heartbeat only after the new JSON is complete. Schema 3 includes:

- process, session, window, time, frame, and draw state;
- ReShade API version and experiment mode;
- compiled unit, LOD, slot, and table-entry count;
- hunt phase, passes, and bytes read;
- exact A/B hit counts and partial-match diagnostics;
- each table address, variant, containing region, size, and page protection;
- automation stage, focus/input completion, error, and capture count;
- target selection, writes, immediate readbacks, present readbacks, refills, stale skips, restoration, and edit errors; and
- separate marker and animated-palette counters so old populations cannot be mistaken for converted-table hits.

Telemetry's `visual_result` field is currently `not_evaluated`. Visual proof remains a separate human-reviewed artifact. A runner status of `passed-converted-edit` proves the exact memory control loop, not by itself the screenshot interpretation.

## Automated runner workflow

[`tools/run_live_test.ps1`](tools/run_live_test.ps1) is both the launcher and the validation harness.

### Preflight and deployment gates

Before deployment or launch it:

1. resolves the game executable, ReShade DLL, profile, and newest built add-on;
2. checks for live and half-terminated `helldivers2.exe` process objects;
3. verifies that the installed patch main/GPU/stream triplet is exactly profile A or profile B;
4. refuses an unknown or marker-expanded patch state;
5. verifies that the add-on is an x64 PE binary;
6. records the ReShade version and add-on SHA-256;
7. rejects edit mode unless input automation is enabled;
8. rejects direct ReShade back-buffer capture and `CopyFromScreen` window capture for this crash-sensitive stage; and
9. deploys the add-on only when the game is stopped and the existing DLL is unlocked.

The runner does not install or swap patch profiles. The operator must place exact A or B in the game data directory before the run. Runtime restoration only restores process memory to the state found at launch; it does not change the on-disk patch.

Use a non-launching preflight with:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_live_test.ps1 `
  -PreflightOnly -NoDeploy -NoLaunch
```

### Process and session identity

If no game is running, the runner starts Steam with application ID `553850`. If a live game is already present, it attaches to that process.

Telemetry is accepted only when its PID matches the tested process and its session ID differs from any previous stopped session. The runner also tracks process start time. Before input and shutdown it rechecks PID plus start time so a restarted process or reused PID cannot be mistaken for the original target.

A heartbeat must be no more than five seconds old, the process must still be live, and the frame counter must be nonzero for `runtime_active` to pass. If frames stop advancing for 20 seconds, the runner stops waiting and reports the stall.

### Input and focus handling

The console and game render window share a PID, so process activation alone is insufficient. The add-on:

1. minimizes its console;
2. obtains the exact ReShade runtime HWND;
3. restores and foregrounds that window;
4. clicks its center once if focus acquisition initially fails; and
5. sends keys only while that exact window retains foreground focus.

When the runner launched the game, the add-on waits 25 seconds before a single 150 ms `W` intro-skip attempt. It then waits the configured scene delay and requires at least 20 draws in the latest frame before starting the test. Repeating inputs aggressively during early loading was removed because the game frequently ignores them and early experiments were unstable.

For a read-only scan run, the stimulus is:

```text
hold W for 3 s -> add D for 2 s -> release -> tap B for 150 ms -> wait -> finish
```

This provides walking, turning, and the stretch emote for observational tests.

For a converted edit comparison, walking and stretch are deliberately skipped. After the ready state the runner waits five seconds in a fixed rear idle view. This prevents normal pose and camera differences from being confused with deformation.

### Fixed-camera edit capture sequence

With `-EditTest -SteamCapture`, the sequence is:

```text
installed A or B idle state
    -> Steam F12 before capture
    -> wait 300 ms
    -> switch all exact hits to the opposite slot state
    -> hold and verify for 1 s
    -> Steam F12 active capture
    -> wait 300 ms
    -> restore exact original slot bytes
    -> wait 500 ms
    -> Steam F12 restored capture
```

The F12 key is released after 100 ms. The runner finds the single Helldivers 2 Steam screenshot directory, records the files that existed before the test, and copies only new screenshots into the timestamped result directory with deterministic names.

Steam capture is currently the accepted path. ReShade back-buffer capture and external `CopyFromScreen` correlated with unstable runs and are blocked by preflight. This is a safety decision based on correlation, not proof that capture alone caused every crash.

### Pass and failure rules

A read-only pass requires:

```text
fresh telemetry from the exact live process
experiment_mode == converted_ib_scan
converted_ib_hits > 0
```

An edit-channel pass additionally requires:

```text
experiment_mode == converted_ib_edit
automation completed
edit completed
at least one successful write
at least one immediate exact readback
all written targets restored
edit_error == 0
```

Current positive statuses are `passed-converted-scan` and `passed-converted-edit`. Distinct failures cover load, runtime heartbeat, experiment-mode mismatch, input, no exact converted match, edit, and shutdown.

Again, `passed-converted-edit` is the machine-verifiable memory result. The final visible-mechanism claim also requires reviewing the before/active/restored images.

### Result collection and shutdown

Each run creates `test-results/<timestamp>/` and records:

- `summary.json`, containing the strongest and final telemetry plus environment, patch, add-on, input, capture, and shutdown state;
- `telemetry.json`, the last accepted heartbeat;
- `reshade-tail.log`, filtered from only the latest 600 ReShade log lines; and
- named captures when requested.

With `-ShutdownAfterTest`, the runner terminates only the exact tested PID after verifying its start time, waits up to ten seconds, and treats an exited-but-still-present process object as a failed shutdown. It never attempts to manage or terminate Steam.

`-RecoverOnly` checks for live and half-terminated game processes, attempts ordinary exact-PID cleanup only for already-exited entries, and verifies that the installed add-on is unlocked. The optional elevated recovery script repeats the same bounded policy. If Windows retains the process, the prescribed recovery is a Windows restart rather than individual thread termination.

## Controlled validation results

### Offline visual control

The first major positive evidence came from static inverse-bind patch edits. A 5 m slot-3 control produced severe geometry extension across the scene. This proved that the patch inverse binds are upstream of visible skinning, but it did not prove runtime control. The magenta material seen in these tests may be a separate material/shader fallback and is not itself evidence of bone movement.

The later slot-7 control was deliberately no-growth and no-reweight. That removed the unstable variables introduced by earlier marker and vertex-remap experiments.

### Controlled run 1: read-only discovery

Result directory: `test-results/20260914-014647`

Process ID: 33320

Installed state: initial 0.15 m profile B

```text
status                       passed-converted-scan
full converted hits          4
full A hits                  0
full B hits                  4
best partial candidate       87 / 88 entries
walking/turn/stretch         completed
shutdown                     succeeded
```

This established that exact loader-converted profile tables could be found read-only. It did not yet establish that rendering consumed them.

### Controlled run 2: initial reversible edit

Result directory: `test-results/20260914-015311`

Process ID: 37444

Installed state: initial 0.15 m profile B

```text
status                       passed-converted-edit
targets selected/written     4 / 4
immediate exact matches      4
present-time readbacks       3,960
refills                      0
targets restored             4
restore succeeded            true
shutdown succeeded           true
```

The byte-level loop passed. Its images were rejected as visual proof because the before image used the stretch pose while the active and restored images used later poses. That protocol flaw directly caused the fixed-camera edit path to be added.

### Controlled run 3: exaggerated fixed-camera B to A to B

Result directory: [`test-results/20260914-015847`](test-results/20260914-015847)

Timestamp: `2026-09-14T01:59:51.3300521+08:00`

Process ID: 23088

Installed state: exaggerated 1.5 m profile B

Add-on SHA-256: `4BAED3AC5342C6ED01C4FA1C3FA4EABDD762DE889F7ADD8C435D60C78FEBF060`

```text
status                       passed-converted-edit
experiment mode              converted_ib_edit
full converted hits          4
full A hits                  0
full B hits                  4
partial candidates           4
best partial candidate       87 / 88 entries
hunt passes                  5
hunt bytes read              16,356,917,120
targets selected/written     4 / 4
immediate exact matches      4
present-time readbacks       532
refills                      0
stale skips                  0
targets restored             4
restore succeeded            true
edit error                   0
marker writes                0
animated-palette hits        0
automation completed         true
shutdown succeeded           true
```

The four exact 4,224-byte tables were inside one writable 64 KiB private allocation:

```text
region base     0x1f00bbe0000
region size     0x10000 / 65,536 bytes
protection      4 / PAGE_READWRITE

table 0         0x1f00bbe0080
table 1         0x1f00bbe1290
table 2         0x1f00bbe24a0
table 3         0x1f00bbe36b0
table stride    0x1210 / 4,624 bytes
table data      0x1080 / 4,224 bytes
```

The exact fact is that four complete profile-B tables appeared consecutively at this regular stride. The likely interpretation is that they correspond to the four 88-entry main LOD table records. Because the offline contents of LODs 0–3 are identical, this run cannot assign a specific LOD number to a specific address. Runtime addresses are process-specific and are not stable identities.

The visual sequence is:

1. [`capture-edit-before-steam.jpg`](test-results/20260914-015847/capture-edit-before-steam.jpg): profile B; slot-7-weighted geometry extends far left from the character.
2. [`capture-edit-active-steam.jpg`](test-results/20260914-015847/capture-edit-active-steam.jpg): exact profile A held at runtime; the extension disappears and the arm/shoulder returns to its normal position.
3. [`capture-edit-restored-steam.jpg`](test-results/20260914-015847/capture-edit-restored-steam.jpg): exact profile B restored; the displaced geometry returns.

The magenta material is incidental. The pass condition is the large spatial B/A/B transition under a fixed camera and idle pose.

Raw evidence is preserved in the result directory:

- [`summary.json`](test-results/20260914-015847/summary.json)
- [`telemetry.json`](test-results/20260914-015847/telemetry.json)
- [`reshade-tail.log`](test-results/20260914-015847/reshade-tail.log)
- [`visual_assessment.json`](test-results/20260914-015847/visual_assessment.json)

## What the positive result proves

- The runtime process contains exact 48-byte-per-slot tables derived from the profiled patch inverse binds.
- These tables can be located without searching animated mapped upload buffers.
- Slot 7's packed `t48` placement and translation conversion are correct.
- At least one visible B-01 chest geometry path consumes the located tables without requiring equipment reload.
- Changing only the exact existing weighted slot changes the rendered mesh.
- Switching all four matching table copies is sufficient for a stable visible effect across the captured state.
- A one-second exact B/A/B control loop can verify, hold, and restore the data without a game crash in the validated run.
- The upload-ring locator and marker-tail write approaches are unnecessary for this one-unit runtime deformation mechanism.

## What remains unproven

- The exact semantic meaning of every one of the 88 slots.
- Runtime-only recovery of names such as `l_shoulder` without offline metadata.
- Parent/child hierarchy and coordinated clavicle/shoulder/twist adjustments.
- Direct per-material and per-vertex ownership for each runtime table.
- Which of the four addresses corresponds to each LOD.
- Whether the same exact matcher works after game updates or with other armor units.
- A profile-independent locator.
- Clean editing of helmet, cape, body, and other skinned components at the same time.
- Safe vertex reweighting or palette growth for a production workflow.
- Arbitrary live translation, rotation, and scale controls derived from user input.
- Automated visual comparison. Screenshot interpretation is still manual.
- Release-level stability. One successful corrected edit run establishes feasibility, not broad soak-test coverage.

## Multiple patches and shared armatures

Sharing animated carrier bones does not imply that every visible component shares one physical inverse-bind table.

Helmet, chest, cape, body, weapons, and other skinned units can have separate:

- unit resources;
- inverse-bind tables;
- palette slot lists;
- mesh-local-to-palette remaps;
- vertex weights;
- material sections;
- runtime table instances; and
- downstream draws.

One patched/profiled unit is enough to prove and control deformation for vertices owned by that unit. It is not enough to adjust every body part. A complete tool will need one discovered mapping per affected skinned unit, or a separately proven higher common control point.

The current four hits should therefore be treated as table copies belonging to the profiled B-01 unit, not as four body components and not as a universal character armature.

## Failed approaches and lessons retained

### Anonymous transform scanning

The first console displayed slots from numerically plausible 48-byte arrays. Values changed during walking and turning, including changes resembling `0 -> -180`. That proved only that live transforms were visible.

Later sweeps applied translations as large as `+/-40`, `+/-80`, and `+/-120` across every slot in dozens of candidates. Thousands of writes and readbacks succeeded, but no player, NPC, or equipment deformation appeared. The candidates were not the render-driving player inverse-bind tables.

Lesson: numerical matrix shape plus motion is not ownership.

### Generic upload-ring targeting

Mapped-buffer experiments found many 48-byte arrays and observed reuse or overwrite. Some runs wrote and restored hundreds of candidates. No credible visible deformation resulted.

Lesson: CPU write/readback proves access, and later overwrite proves reuse, but neither proves that the target draw consumed that copy.

### Natural `file64` fingerprint

A 1,024-byte source-patch prefix appeared once in several gigabytes of process memory. It could be written, read back, overwritten, and restored, but the Steam frames did not change.

Lesson: a source-format inverse-bind occurrence may be private reference data or an already-consumed source copy. The loader-converted `t48` object is the effective live layer.

### Marker-tail upload-ring discovery

A marker-only patch added a repeated unused tail without reweighting vertices. It ran stably for about 85 seconds and its marker appeared in hundreds of upload-ring copies. Editing selected copies produced millions of exact readbacks but no visual effect; they were stale or unrelated to the captured draw.

Lesson: an offline marker is useful only when its runtime destination and consumption point are identified. Repetition alone is not enough.

### Palette growth and vertex reweighting

Several patches grew or reused palette slots and rewrote 9,596 vertex bone-index bytes. They passed 228–236 structural assertions but repeatedly crashed or TDR-failed in game. Marker-only growth without reweighting was stable.

Lesson: parser consistency is not semantic validation. Mesh-local indices, palette remaps, material streams, shader limits, and fake/real bone mappings cannot be assumed interchangeable. Reweighting remains paused.

### Capture paths

External `CopyFromScreen` and some earlier in-add-on capture work correlated with D3D device removal. Steam F12 screenshots worked in the stable corrected runs.

Lesson: use Steam capture for this validation stage and do not claim one exclusive crash cause without matching crash-event, patch, add-on, and mutation records.

### Misleading result labels

Earlier statuses such as `passed-edit-channel` and `passed-with-motion` sounded stronger than their evidence. They meant writable memory or moving anonymous matrices, not visible deformation. One old aggregate restoration Boolean could also pass when only a subset of targets restored.

Lesson: pass criteria must correspond to the claim. Current converted edit restoration requires all written targets, and visual success is recorded separately.

## Crash and recovery findings

The historical crashes include at least two failure families:

1. D3D device removal/TDR evidence including `DXGI_ERROR_DEVICE_HUNG` (`0x887A0006`) and D3DDRED2 records during the risky scanner/capture/patch period.
2. BEX64/`ntdll` failures with `0xc000000d` or `0xc0000026`, including failures before a scan or write began.

It is not defensible to say that every crash had one cause. The evidence supports only narrower conclusions:

- self-mapping or aggressively reading D3D12 resources was unsafe and was removed from the active path;
- external window capture correlated with instability and is blocked;
- reweighted patch variants were unstable while marker-only growth was stable;
- some startup failures occurred before any mutation; and
- the final corrected exact-table run wrote and restored all four targets, captured all three states, shut down cleanly, and did not crash.

The runner always re-queries `helldivers2.exe`; it does not assume the user has not intervened. A half-terminated process or locked add-on blocks deployment. Recovery is exact-PID and bounded. Steam state is left to the user.

## Build and verification workflow

### Build dependency

The only build-time external dependency is the official ReShade 6.5.1 SDK, pinned at revision:

```text
f1332dfe8fb1c61a726d53af069cc2a2fcacae7f
```

Prepare it from the repository root:

```powershell
git clone --depth 1 --branch v6.5.1 --filter=blob:none --sparse `
  https://github.com/crosire/reshade.git deps/reshade
git -C deps/reshade sparse-checkout set --no-cone /include/ /LICENSE.md
```

Build and test:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The DLL is produced at:

```text
build/bin/Release/HD2PaletteProbe.addon64
```

The current focused test suite contains:

- `addon_loads_and_registers`: loads the add-on and verifies callback registration;
- `inverse_bind_layouts`: verifies nontrivial `file64`/`t48` conversion, translation indices, affine inversion, and bind-space identity; and
- `converted_profile_scan`: plants multiple dynamic tables among slot-anchor decoys and verifies matching and exclusion rules;
- `runtime_profile_format`: validates package parsing and corruption rejection; and
- `runtime_profile_pipeline`: constructs a synthetic patch and validates extraction and LOD deduplication.

All five pass in the external-profile implementation.

### Read-only live validation

With exact profile A or B already installed:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_live_test.ps1 `
  -GameRoot "F:\Steam\steamapps\common\Helldivers 2" `
  -ObserveSeconds 90 -AutomateInput -SteamCapture -ShutdownAfterTest
```

This uses movement and stretch stimuli but performs no converted-table write.

### Reversible live edit validation

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_live_test.ps1 `
  -GameRoot "F:\Steam\steamapps\common\Helldivers 2" `
  -ObserveSeconds 90 -AutomateInput -EditTest -SteamCapture -ShutdownAfterTest
```

This invokes the fixed-camera exact opposite-profile pulse. It should be used only with a deliberately controlled patch and after a preflight pass.

## Current deployment state

The final preflight after the successful run reported:

```text
ReShade              6.5.1
add-on architecture  x64
installed patch      exact profile A
installed add-on     4BAED3AC5342C6ED01C4FA1C3FA4EABDD762DE889F7ADD8C435D60C78FEBF060
helldivers2.exe      stopped
```

Runtime memory was restored to the launch-time profile before shutdown. After evidence collection, the on-disk B-01 triplet was separately restored to exact profile A, so a later manual launch uses the normal geometry.

No automation request is intentionally left pending. The deployed add-on defaults to read-only scanning unless a new request explicitly enables the edit test.

The private GitHub key remains ignored and is not part of the repository.

## Current product boundary

The current build is a mechanism-validation prototype, not a release armature editor.

It has:

- a standalone console;
- a profile-specific exact table locator;
- a safe bounded one-slot A/B edit pulse;
- heartbeat telemetry;
- input, capture, shutdown, and crash-recovery automation; and
- reproducible evidence for one B-01 chest slot.

It does not yet have:

- a clean list of all converted slots and their matrices;
- a standalone interactive control console;
- arbitrary translation/rotation/scale input;
- semantic slot names reconstructed by this project;
- hierarchy-aware editing;
- vertex influence lists;
- a generalized unit/profile registry;
- multi-component coordination; or
- a stable end-user installation package.

The old anonymous candidate display should not be mistaken for the future slot interface. A clean interface should display only tables that have passed unit/profile ownership validation.

## Development workflow and current milestone status

### Milestone 1: extract the verified core

Status: completed by the external-profile refactor.

- Remove or isolate anonymous scanning, marker-tail upload code, hard-coded draw-index filters, and old residency probes.
- Keep the `file64`/`t48` codec, exact profile scanner, telemetry, edit/restore guard, and runner.
- Preserve read-only mode as the default.

Exit condition: the smaller add-on reproduces the same four hits and B/A/B edit result without legacy counters or write paths.

### Milestone 2: expose a clean table and slot interface

- Enumerate validated table instances.
- Display unit ID, profile state, table address for diagnosis, slot count, and every slot's decoded matrix.
- Show translation at `t48[3/7/11]` and a stable logical key separate from the transient address.
- Add read-only change observation while vanilla animation runs.

Exit condition: the console consistently reports the same validated unit slots without anonymous false positives.

### Milestone 3: add bounded arbitrary editing

Status: translation editing completed and live-validated; rotation and scale are not implemented.

- Replace exact A/B switching with a small user-selected transform delta.
- Compose the desired adjustment using the verified matrix convention rather than modifying guessed float positions.
- Start with translation only, one slot, clamped range, explicit apply, and exact undo.
- Keep a shadow copy and refuse to write if the current table no longer matches the expected state.

Exit condition: multiple translation magnitudes produce proportional visible changes and exact restoration across repeated runs.

### Milestone 4: reconstruct semantic mapping offline

For each supported unit, extract and preserve:

- scene-graph hierarchy;
- node indices and name hashes;
- inverse-bind matrices and derived bind-space positions;
- material-section bone remaps;
- vertex bone indices and weights;
- LOD-specific palette layouts; and
- the minimum fingerprint needed to locate runtime tables.

A development hash dictionary may validate names, but it should not be a required runtime dependency. The public identity should resemble:

```text
unit -> LOD/material -> palette binding -> slot -> influenced vertices
```

Exit condition: `slot 7` can be derived and presented as a stable shoulder mapping by this project's own metadata pipeline, including the vertices it influences.

### Milestone 5: support coordinated and multi-unit adjustments

- Determine shoulder/clavicle/upper-arm/twist relationships.
- Apply hierarchy-aware related-slot edits only after individual controls remain stable.
- Locate separate tables for chest, body, helmet, cape, and other components that must follow the adjustment.
- Treat an absent or mismatched component as a reported partial application, not as permission to scan and write anonymous memory.

Exit condition: one logical adjustment produces consistent geometry across every supported visible component and LOD.

### Milestone 6: harden for repeated use

- Add versioned profile packages and game-update mismatch detection.
- Reduce the multi-gigabyte exact scan with a safe, profiled locator or bounded allocation discovery.
- Add repeated launch/edit/restore/LOD/equipment-reload soak tests.
- Automate image registration or difference scoring while keeping a human-verifiable frame triplet.
- Package installation, uninstall, log collection, and recovery without managing Steam.

Exit condition: the tool fails closed on unknown versions, restores safely, and remains stable across a meaningful test matrix.

## Final current-stage conclusion

The original idea was partly correct but needed one architectural correction.

It is feasible to alter visible skinned geometry at runtime by changing inverse-bind relationships while vanilla animation continues. The successful location is the loader-converted private `t48` table, not an arbitrary animated matrix array and not necessarily a downstream upload-ring copy.

Pure runtime semantic armature reconstruction is not a dependable basis for the product. A small offline unit-analysis/profile stage is required to establish unit ownership, slot identity, hierarchy, remaps, and vertex influence. That stage does not need VRM or the reference project's runtime. The prototype now loads versioned external profiles containing the inverse-bind tables extracted from each finished patch; semantic hierarchy, remaps, and vertex influence are not yet part of the format.

The next justified step is to extend the offline profile with semantic hierarchy, material remaps, and vertex influence, then expose stable human-readable controls above the validated unit-and-slot edit channel.
