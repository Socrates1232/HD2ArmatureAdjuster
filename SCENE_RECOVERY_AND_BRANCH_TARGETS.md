# Scene recovery and shoulder-branch targeting

## Runtime state

F8 intent and table ownership are separate states:

```text
desired OFF
    F8
desired ON, waiting
    valid pristine instance discovered
desired ON, applied
    instance disappears or becomes unknown
desired ON, waiting
    replacement instance discovered
desired ON, applied
    F8
desired OFF, guarded restore
```

`desired ON` is never cancelled by a failed scan or a stale address. The add-on
keeps a registry of `(address, exact profile table, memory region, last-seen
generation)` instances. Owned addresses are validated on every present. Three
consecutive unreadable/unknown samples retire ownership but preserve intent.

Discovery has two scopes:

- productive memory regions are rescanned every two seconds;
- resource churn schedules a full scan after one quiet second;
- a full fallback scan runs every 30 seconds while F8 remains requested ON.

The full pass walks the entire process address space but reads only committed,
writable, private 64 KiB allocations. Every confirmed table found in prior live
tests occupied that allocation class. This keeps discovery off the multi-gigabyte
heaps whose scanning coincided with a D3D device-removal failure. The discovery
thread also runs below normal priority.

Every discovery generation is merged into the registry. Existing instances are
not discarded merely because a later scan found nothing.

## Write ownership

Before a write, the complete table is classified as:

- `PRISTINE`: exactly equal to the profile table;
- `OUR_OVERRIDE`: exactly equal to the full table this add-on expects;
- `UNKNOWN`: anything else.

Only `PRISTINE` is changed. `OUR_OVERRIDE` can be adopted or maintained.
`UNKNOWN` is skipped. Disable restores configured slots only when the complete
table is `OUR_OVERRIDE`; failed restore verification never reinjects the edit.

## Offline shoulder expansion

Run profile generation first, then:

```powershell
python tools/patch_profile_tool.py shoulder-targets `
  --root "path\to\converted-mod-copy" `
  --profile-dir "path\to\converted-mod-copy\HD2ArmatureProfiles"
```

The generator hashes `l_shoulder` and `r_shoulder`, walks each unit's parent
links, includes both roots and every descendant, and maps nodes to palette slots
through every LOD's `RealIndices`. Rows use:

```text
unit_id table_fingerprint slot x y z
```

The table fingerprint is the FNV-1a value already embedded in `.hd2profile`.
This is necessary because one unit's numeric slot can identify different nodes
in different LOD tables. If byte-identical runtime tables assign the same slot
conflicting semantics, generation fails instead of emitting an unsafe mapping.

The source patches are read only. `shoulder_targets.txt` and its JSON audit
report are written into the isolated profile directory. The runner deploys both
the active profile union and this target map with the game stopped.

## Current SR-24 validation bundle

The generated bundle scanned 79 patch files, observed 22 active armature-table
variants, and emitted 777 qualified rows: 381 left and 396 right. The currently
installed replacement contributed 347 rows, all present in that generated set
with no side/sign conflicts.

The final API-17 smoke run registered successfully, loaded 19 active profile
files as 27 unique runtime tables with zero profile/config errors, parsed all
777 targets, completed the scripted walk/turn/stretch sequence, and shut down
cleanly. Candidate scanning read about 384 MiB instead of the earlier 3.4 GiB
full-heap pass, and no new Windows crash event occurred. The equipped model in
that smoke run produced no exact table match. The subsequent user-assisted test
confirmed visible bilateral descendant deformation, persistence across a scene
change, automatic rebinding, and F8 restoration.

The acceptance test used deliberately exaggerated `+0.25 m` left and `-0.25 m`
right translations. After validation, the default deployment preset was reduced
to `+0.03 m` left and `-0.03 m` right, approximately 6 cm total narrowing. These
remain uniform bind/model-space pre-offsets across each branch, not a final
pose-aware shoulder-width algorithm.

## Automated checks

The test suite covers:

- complete-table state classification;
- instance-registry merge and relocation retention;
- synthetic shoulder/descendant expansion through two differently ordered LODs;
- rejection when an identical runtime table reuses a slot with conflicting bone
  semantics;
- add-on load/register/unload and the existing profile/scan/layout pipelines.

Acceptance status: passed. One F8 press produced obvious bilateral branch
displacement, a scene transition retired the old table instances, new instances
were discovered and edited without another F8 press, and the second F8 restored
the unmodified appearance.
