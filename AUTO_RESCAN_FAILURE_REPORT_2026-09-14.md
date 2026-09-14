# Automatic Reacquisition Experiment Failure Report

Date: 2026-09-14  
Experiment build: HD2 Armature Profile Runtime 0.9  
Last known working source baseline: commit `04c0fe7` (runtime behavior unchanged from `d16c443`)  
Installed 0.9 SHA-256: `AB615CC8FC6B198FA540CDACC0801522ECA858307F3FFD3E1BC57B857743FAB2`

## Outcome

The 0.9 F8 experiment produced no visible shoulder edit. This run does not show
a fingerprint, profile, or shoulder-map failure: the scanner found the expected
shoulder tables. It shows that the new scan-before-write control flow never
entered the edit stage.

The automatic reacquisition approach should therefore be detached from F8 and
tested read-only before it is allowed to control runtime writes. The installed
0.9 build is an unsuccessful experiment, not a new stable baseline.

## User-observed sequence

1. The earlier F8 edit worked before a scene change.
2. The deformation did not survive the scene change.
3. Subsequent toggling stopped producing a visible result.
4. Version 0.9 was built to force a fresh shoulder-only scan on F8 and to
   reacquire targets after three consecutive invalid reads.
5. With 0.9 installed, F8 produced no visible effect at all.
6. The game was stopped normally before evidence collection.

## Captured evidence

The final telemetry snapshot was schema 6 from process 33780:

| Field | Value |
|---|---:|
| Profile files loaded | 19 |
| Unique profile tables loaded | 27 |
| Profile load errors | 0 |
| Shoulder targets configured | 4 |
| Exact shoulder-table hits | 12 |
| Hunt phase | 2 (found) |
| Hunt passes | 1 |
| Bytes scanned | 3,914,711,168 |
| Partial candidates | 114 |
| Best partial entries | 86 |
| F8 toggle count | 12 |
| Shoulder pending | false |
| Shoulder active | false |
| Automatic reacquires | 0 |
| Edit targets selected | 0 |
| Edit targets written | 0 |
| Write attempts | 0 |
| Edit errors | 0 |

The 12 exact matches comprised four copies of each required 88-entry unit:

- `dd66041f2f232ac3` — torso undergarment
- `adb5d7b476e79121` — left arm undergarment
- `2ce2b973e335f79c` — right arm undergarment

All matches were concentrated in two writable 64 KiB regions:

- `0x26a98be0000`
- `0x26a98c60000`

The ReShade log contains no add-on load error or crash for this run and records
a normal add-on unregister and process exit.

## Confirmed conclusions

1. The installed SR-24 profile union loaded successfully.
2. `shoulder_targets.txt` loaded successfully.
3. All three mapped shoulder units were found by complete exact-table matching.
4. The table variants and target slot bounds were sufficient for discovery.
5. No runtime edit was attempted, so this run does not test whether the actual
   48-byte shoulder writes remain effective.
6. The scan-before-write design added a multi-gigabyte delay to an operation the
   user expects to behave like an immediate toggle.

## Most likely failure mechanism

Version 0.9 made F8-on request a new scan. While a request is pending, another
F8 transition cancels it. The final even toggle count of 12, combined with zero
selected targets and a completed scan containing 12 valid hits, is consistent
with pending requests being cancelled before `begin_edits` could consume the
results.

This is an inference from telemetry rather than a directly logged transition
history. Regardless of the exact press timing, the decisive result is that the
write stage was never reached.

## Why the first scene-change design was incomplete

The original runtime cached raw virtual addresses. It could maintain an edit
while those addresses remained valid and could reapply it if the game restored
the original bytes at the same address. It did not automatically discover a
new allocation after a scene change.

The 0.9 stale-read detector only helps when an old address becomes unreadable or
contains unexpected bytes. If an obsolete table remains resident and still
contains the injected bytes, it looks healthy even when the renderer has moved
to a different copy. Address health is therefore not equivalent to active
character ownership.

## Required fallback

Use commit `04c0fe7` as the behavioral fallback for the next controlled test.
That baseline applies F8 immediately to already discovered hits and had produced
visible deformation. Do not treat the installed 0.9 binary as stable.

The fallback should be performed explicitly in a later implementation/test turn;
no source or installed-file rollback was performed while preparing this report.

## Next experiment: read-only reacquisition

The next implementation should separate discovery from edit state:

1. Restore the known-working immediate F8 path.
2. Add a persistent `desired_on` state that is independent of scan state.
3. Run reacquisition without writing and log old versus newly found addresses.
4. Search the previously successful 64 KiB regions first; fall back to one full
   process scan only when necessary.
5. Keep the existing deformation active while background discovery runs.
6. After a scene change, verify that the read-only scanner finds a genuinely new
   table address or generation.
7. Only after that result, allow the new targets to enter the guarded write path.

Useful telemetry for that experiment:

- desired shoulder state
- discovery generation
- scan reason (`initial`, `manual`, `stale`, or `scene-check`)
- fast-region bytes scanned and full-scan bytes scanned
- old, retained, new, and retired target counts
- target addresses grouped by memory region
- whether any write path was enabled

## Validation gates

1. **Fallback:** confirm the old F8 path visibly edits the current scene.
2. **Read-only relocation:** change scenes and confirm that discovery reports new
   addresses without performing writes.
3. **Controlled attach:** enable writes only for newly discovered exact tables.
4. **Persistence:** confirm the desired edit returns after the scene change.
5. **Disable:** confirm one F8-off action restores every still-valid injected
   target and leaves no pending scan capable of reapplying it.

No additional runtime-code changes were made after the failed 0.9 test was
reported. This document records the frozen experimental state for review.
