# Dynamic Runtime Profile Validation

Date: 2026-09-14

## Result

The hardcoded B-01 inverse-bind data was removed from the add-on and replaced with an external `HD2IBP1` profile extracted from the installed patch. The new path passed both runtime identification and reversible editing.

## Source identity

Installed main patch:

```text
9ba626afa44a3aa3.patch_0
SHA-256 fb181c4f3d4beb6500eef50a0a94977b20a7388252dd4925265eb0d504f23d23
```

Generated profile:

```text
profiles/runtime/9ba626afa44a3aa3.patch_0.hd2profile
4,720 bytes
SHA-256 cecd2a3cb8203f52c2506a8c5a2e9e55c061d8bb9d966352aac730ff77354b86
```

The package contains one unit and three unique records:

| Unit | LOD mask | Entries | Packed-table SHA-256 |
| --- | ---: | ---: | --- |
| `fa269172bd08695b` | `0x0000000f` | 88 | `13941f8e263792a830854eece1ac61a432e2dba79dc2a30b8cdac24282b2740b` |
| `fa269172bd08695b` | `0x00000010` | 2 | `b4cdad75b8f25b79901abecf9246caab3b603982bca7f1ab2896513a7dd0ba8d` |
| `fa269172bd08695b` | `0x00000020` | 4 | `df6113ba8d9f0b1ff55d474c47cacdb59ee04cdbc96b04c405b7b6ca7629dba4` |

The extracted 4,224-byte 88-entry table has exactly the same SHA-256 as profile A in the legacy compiled header. This independently rules out a matrix-conversion or source-selection change during the refactor.

## Live runs

### Run `20260914-090857`

- one profile file and three records loaded;
- zero profile errors;
- no exact table became resident during the observed startup;
- the game exited before the input/edit stage;
- no memory write was attempted.

This was a safe negative run, not evidence of a profile parse failure.

### Run `20260914-091715`

- one profile file and three records loaded;
- zero profile errors;
- four exact converted inverse-bind table hits found;
- zero partial candidates;
- the game exited before the conservative draw-count readiness gate;
- no memory write was attempted.

This establishes that the external profile reproduces the legacy locator's four-hit result.

### Run `20260914-091922`

Request:

```text
unit fa269172bd08695b
slot 7
world translation [-1.5, 0, 0] metres
```

Observed:

| Counter | Value |
| --- | ---: |
| Exact table hits | 4 |
| Targets selected | 4 |
| Targets written | 4 |
| Successful writes | 4 |
| Immediate readbacks | 4 |
| Present-time injected readbacks | 588 |
| Exact source refills | 0 |
| Stale/third-state skips | 0 |
| Targets restored | 4 |
| Restore succeeded | true |
| Edit error | 0 |

The runtime was active, automation completed, and the edit channel passed. No Steam capture was requested in this isolation run.

The report's outer status was `failed-shutdown`: after exact-PID termination, Windows briefly retained the process object even though it cleared before the next manual check. The edit had already restored all targets before shutdown. The runner now waits through this short cleanup interval before declaring a shutdown failure.

## Interpretation

The external profile pipeline has crossed the intended validation gates:

1. finished patch to deterministic profile;
2. add-on loads the profile without compiled patch bytes;
3. exact runtime identification reproduces the known four-table result;
4. unit-and-slot selection resolves those targets;
5. arbitrary world-space translation is converted to bind-space packed bytes;
6. all writes and immediate readbacks succeed;
7. the injected state remains observable across present callbacks; and
8. every changed target restores exactly.

This validates the profile boundary and runtime edit channel. It does not add semantic bone naming or vertex ownership; those remain separate offline mapping work.

## Load-time union regression

Run `20260914-093828` loaded two separately named copies of the same package to exercise composition and exact cross-file deduplication:

| Counter | Value |
| --- | ---: |
| Profile files | 2 |
| Source table records | 6 |
| Duplicate records merged | 3 |
| Unique runtime tables | 3 |
| Exact runtime hits | 4 |
| Partial candidates | 0 |
| Targets written/restored | 4 / 4 |
| Present-time readbacks | 600 |
| Profile/edit errors | 0 / 0 |
| Exact-process shutdown | succeeded |

The result was `passed-converted-edit`. This demonstrates that profile files remain patch-level provenance artifacts while the add-on operates on a deduplicated per-unit/per-table union. Duplicate inputs did not double the scan hits or write targets.
