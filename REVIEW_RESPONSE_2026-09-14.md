# Converted Inverse-Bind Review Response

Date: 2026-09-14
Scope: response to the review of `TEST_REPORT_2026-09-14.md` and preparation for the next controlled experiment

## Outcome

The review identified a real architectural error in the runtime experiments. The earlier code treated three different objects as if they were interchangeable:

1. the 64-byte inverse-bind matrices stored in the patch (`file64`);
2. the loader-converted 48-byte inverse-bind table (`t48`);
3. temporary 48-byte animated skinning matrices in mapped upload buffers.

The reference implementation treats the converted `t48` table as its preferred live bind-space control point. The previous runtime tests did not knowingly locate or edit that object. Therefore, none of their successful write/readback results prove runtime armature control.

No new game run was performed for this response. Work stopped at offline profile validation, a read-only scanner build, and runner safety gates.

## Corrections to the earlier interpretation

- An offline `file64` edit can work because the game converts the already-modified data while loading the unit. Editing the original `file64` allocation after conversion need not affect the instantiated character.
- A moving 48-byte array in an upload ring is downstream animation output. It is not thereby the converted inverse-bind source.
- CPU write/readback proves only that an address was writable. It does not prove that the engine later consumed that address.
- The magenta-corruption capture is not positive evidence of bone deformation. It proves that an active write reached render-relevant data, but the affected data was not identified as an inverse-bind table.
- The anonymous slots that changed during movement remain useful evidence that animated matrices were observed, but not that the correct edit layer was found.
- Previous `ib_hit_count`-style reporting mixed inverse-bind and upload-ring targets. The new telemetry names those populations separately.
- Previous residency probes performed mutations that were not represented by the edit counters. Marker writes and refills now have their own counters.
- The crash cluster remains correlated with experimental writes and capture paths, but the evidence does not isolate one cause. The read-only stage excludes both add-on/window capture and all edit modes.

The complete 69-run history remains in `TEST_REPORT_2026-09-14.md`; it should be read as an experiment ledger, not as proof that the runtime write target was established.

## Verified matrix representation

For a row-vector affine `file64` matrix with floats `m[0..15]`, the reference conversion is:

```text
t48 = [
    m0, m4, m8,  m12,
    m1, m5, m9,  m13,
    m2, m6, m10, m14
]
```

The translation components of `t48` are therefore indices `3`, `7`, and `11`. The earlier generic 48-byte helper used indices `9`, `10`, and `11`; that layout was incorrect for this converted inverse-bind representation.

`src/ib_layout.hpp` now defines explicit `file64` and `t48` codecs. `tests/ib_layout_test.cpp` verifies:

- conversion and round trip with nontrivial rotation, nonuniform scale, and translation;
- the exact `3/7/11` translation positions;
- affine inversion;
- the bind-space identity `(T * IB) * W == T`;
- conversion of every entry in the generated B-01 A/B profile.

## Test A: exact offline A/B control

The control changes one existing, weighted B-01 chest slot and does not grow the palette or alter vertex weights.

| Field | Value |
| --- | --- |
| Bundle | `9ba626afa44a3aa3.patch_0` |
| Unit | `fa269172bd08695b` |
| Slot | `7` (`l_shoulder` according to the existing census) |
| Operation | world-space displacement `(-0.15, 0, 0)` metres |
| Modified LODs | 0, 1, 2, 3 |
| Profile LOD | 0, 88 entries, 4,224 converted bytes |
| Growth/reweighting | none |

Profile A is the original B-01 chest patch:

```text
main   fb181c4f3d4beb6500eef50a0a94977b20a7388252dd4925265eb0d504f23d23
gpu    a4cb635c27ff4ca0056df2a5ab9caaef7cabd3aafc698038b01ce962cb10fd1f
stream e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

Profile B is the displaced control:

```text
main   fabd2c95511310686b0ee4e4531dc733d18106b505b34af3a19cb1c54913a16f
gpu    a4cb635c27ff4ca0056df2a5ab9caaef7cabd3aafc698038b01ce962cb10fd1f
stream e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

The patch generator passed all 83 structural and semantic gates. Main-file size and layout are unchanged. The GPU and stream files are byte-identical. The only changed matrix fields are slot 7's translation row in LODs 0 through 3; 40 individual bytes differ because several changed floats retain some identical bytes. LODs 4 and 5 have too few entries to contain slot 7 and remain unchanged.

`tools/build_ib_profile.py` independently parses both triplets, validates those restrictions, converts all inverse binds, and emits:

- `profiles/b01_slot7_ab.json`, the reviewable evidence/profile file;
- `src/ib_profile_data.hpp`, the exact LOD-0 A/B `t48` tables used by the scanner.

The generated profile stores only patch basenames, not local machine paths.

## Test B implementation: read-only converted-table discovery

The add-on is now compiled in the explicit `converted_ib_scan` mode. Its scanner:

- searches committed, writable, private process memory;
- uses slot 7 as an anchor but accepts a hit only when the complete 88-entry table exactly matches profile A or B;
- scans on four-byte boundaries;
- reports anchor-only candidates separately as partial diagnostics;
- excludes the add-on image and its scan buffer, preventing self-matches against compiled profile data;
- excludes ranges already tracked as mapped graphics buffers, keeping downstream upload memory out of this population;
- performs no marker write, deformation write, or capture.

The original hunter setup defect is also fixed: `ensure_ib_hunt_thread()` now creates the worker thread.

This exact match is deliberately conservative. A negative result would not prove that converted inverse binds are absent. Runtime reordering, canonicalization, table extension, per-instance modification, a different memory type/protection, or loading a different B-01 component could all require a read-only follow-up. The partial-match count and best matching-entry count are retained for that diagnosis.

## Offline scanner tests

`tests/profile_scan_test.cpp` plants exact A and B tables among many slot-7 anchor decoys. It verifies that:

- decoys do not consume the exact-hit limit;
- exact A and B are classified separately;
- mapped/upload ranges are excluded;
- the scanner's own reference-storage range is excluded.

The current build passes all three CTest cases:

```text
addon_loads_and_registers  PASS
inverse_bind_layouts       PASS
converted_profile_scan     PASS
```

The PowerShell runner also passes a parser-only syntax check.

## Runner and evidence changes

`tools/run_live_test.ps1` now refuses to launch unless the installed B-01 triplet hashes exactly match profile A or profile B. It records the installed state and all three hashes in the result. It selects the newest built add-on rather than preferring a possibly stale `build` directory.

For this stage, the runner accepts only Steam screenshots. Add-on back-buffer capture and direct window capture are rejected. `-EditTest` is rejected because `converted_ib_scan` is read-only.

The success condition is no longer anonymous matrix motion. A pass requires:

```text
experiment_mode == converted_ib_scan
converted_ib_hits > 0
fresh telemetry from the exact live process
```

Telemetry now distinguishes:

```text
file64_reference_hits
converted_ib_hits
animated_palette_hits
marker_write_count
marker_refill_count
target_write_count
cpu_match_count
downstream_signature_match_count
visual_result
```

`edit_refills_observed` is read from its own field instead of being copied from present readbacks.

## Current installed-state interlock

At the time of this response, the installed main patch is:

```text
f66bd35b3af01b7a5e1a4851bfb0453200716cc3f75b44621daf423f6ca8f731
```

Its size is 80,904 bytes. This is the earlier marker-expanded patch, not exact A or B. The new runner will therefore stop at preflight and will not deploy or launch the game. This is intentional: an 88-entry prefix inside an expanded table could otherwise create a misleading match.

## Multiple armor components

The character's components can share animated carrier bones without sharing one inverse-bind table. Each skinned unit has its own slot list, remaps, and inverse binds. A fingerprint/profile in the chest patch locates chest-unit instances only. Editing every visible body component would require locating the corresponding table for each affected unit, or proving a higher common control point. One patched component is sufficient for the present one-slot mechanism test, but not for a future whole-body adjustment.

## Next controlled decisions

1. Install and visually verify exact profile A and exact profile B with the same B-01 armor, scene, camera, and LOD. This establishes the offline visual control before any live write.
2. Run the read-only `converted_ib_scan` separately against exact A and B. Accept only full-table hits; use partial diagnostics to refine the representation or allocation filters if needed.
3. Only after Test B succeeds, add a bounded `converted_ib_edit` mode that switches the located slot between the exact A and B bytes, holds each state long enough to observe, and restores A.
4. Treat the upload ring only as a downstream observer. A later marker-propagation test should use unweighted entries and a pose-independent matrix relationship; it must not replace the weighted-slot visual Test C.

Semantic armature reconstruction and vertex reweighting remain paused until the existing weighted slot can be switched visibly at the verified upstream target.
