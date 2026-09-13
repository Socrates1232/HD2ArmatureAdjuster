# HD2 Armature Adjuster: Full Experimental Report

> **Post-review correction:** this report records the first 69 runs, but its runtime experiments did not distinguish the loader-converted `t48` inverse-bind table from downstream animated 48-byte upload copies. No runtime write in this ledger proves control of the reference implementation's preferred target. Read [`REVIEW_RESPONSE_2026-09-14.md`](REVIEW_RESPONSE_2026-09-14.md) for the corrected model and next controlled tests.

Report date: 2026-09-14

Covered evidence: initial add-on work through live run `20260914-004901`

Game: Helldivers 2

Runtime: ReShade 6.5.1, add-on API 17

## Executive verdict

The project has proved four useful things:

1. The ReShade add-on loads in Helldivers 2 and can observe changing transform-shaped data in CPU-visible D3D12 buffers.
2. The runner can focus the game, send `W`, `W+D`, and `B`, collect telemetry, request Steam screenshots, and shut down the exact tested process.
3. An offline edit to the B-01 chest inverse-bind data causes a drastic rendered change. The inverse-bind data in the patch is therefore genuinely upstream of the rendered mesh.
4. A marker-only palette-growth patch loads and runs stably, and its repeated tail appears many times in mapped D3D12 upload memory.

The project has **not** proved its central runtime claim: no add-on write has yet produced a verified visible deformation of the character. The current code can locate and modify memory that resembles the target data, but it has not located the particular fresh upload-ring copy consumed by the relevant character draw.

Several earlier conclusions were too optimistic:

- A moving transform-shaped array was treated as a likely skinning palette. The exaggerated anonymous sweeps disproved that for the tested candidates.
- Successful write/readback/restore was treated as an “edit-channel pass.” It proves CPU memory access only, not render ownership.
- An engine overwrite after a write was treated as evidence of a live render path. It proves reuse of the allocation, but not that the matching character draw consumed that copy.
- A repeated marker tail anywhere in an upload heap was treated as sufficient identification. The marker occurs in hundreds of historical ring copies, most of which are stale.
- Runner status names such as `passed-edit-channel` and `passed-with-motion` were allowed to sound stronger than their evidence.
- Offline structural validation was treated as enough to trust reweighting. The reweighted patches repeatedly crashed even though their offline gates passed.

The correct current status is:

> Offline render control is positive. Runtime render control is unproven. The live-copy locator and timing model are the blocking problems.

No further game test was performed while preparing this report.

## Evidence standard used in this report

To avoid repeating the earlier category error, results are separated into levels:

| Level | Evidence | What it proves |
| --- | --- | --- |
| 0 | Add-on registration and heartbeat | The DLL loaded and callbacks ran |
| 1 | Values changed during gameplay | Live transform-like memory was observed |
| 2 | Write plus immediate readback | The CPU-visible allocation was writable |
| 3 | Later engine overwrite/refill | The allocation was reused or refreshed |
| 4 | Before/active/restored visual difference | The modified bytes affected the rendered frame |
| 5 | Draw/mesh/slot ownership | The edited location is mapped to a specific skinned unit and vertices |

The anonymous runtime experiments reached levels 1–3 but not level 4. The natural inverse-bind fingerprint reached level 3 but not level 4. The marker upload-ring experiments reached level 2 and observed ring reuse, but did not verify the edited copy at level 4. The offline static control reached level 4. No runtime experiment reached level 5.

## Scope and test subject

The principal offline target was the B-01 Lean chest unit:

- Archive: `9ba626afa44a3aa3.patch_0`
- Unit file ID: `fa269172bd08695b`
- Original palette sizes: `88, 88, 88, 88, 2, 4`
- Main patch SHA-256: `FB181C4F3D4BEB6500EEF50A0A94977B20A7388252DD4925265EB0D504F23D23`
- GPU resources SHA-256: `A4CB635C27FF4CA0056DF2A5AB9CAAEF7CABD3AAFC698038B01CE962CB10FD1F`
- Stream SHA-256: `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`

The runner stored 69 timestamped summaries and 241 capture files locally. The `test-results` directory is intentionally ignored by Git, so the complete compact ledger is reproduced below. Early summary schemas did not contain every later counter; missing values are reported as unavailable rather than interpreted as zero.

## What the reference mechanism actually implies

For a palette slot `k`, the reference work indicates a relationship of the form:

```text
H_k = IB_k * W_node(k)
```

where `IB_k` is static inverse-bind data and `W_node(k)` is the current animated carrier-node transform. To request a skinning transform `S_k`, an adjusted inverse bind can be derived as:

```text
IB'_k = S_k * inverse(H_k) * IB_k
```

The important operational detail is not just this equation. The game repeatedly copies a packed 48-byte runtime palette into CPU-visible D3D12 upload-ring memory. A runtime tool must modify the fresh copy after the game fills it and before the GPU consumes it. Finding an old copy with the correct bytes is insufficient.

The reference patch grows a palette and creates a repeated tail whose slots point at a known scene-graph node and use a known inverse bind. This is a locator fingerprint, not a universal armature shared as one physical table by every body part.

Helmet, chest, cape, body, and other skinned units can share animated carrier nodes while still having separate inverse-bind tables, material remaps, vertex weights, upload copies, and draw calls. One patched unit is enough for a proof-of-concept on that unit. A complete editor must identify and update each relevant skinned unit or deliberately patch every component it intends to control.

## Offline patch experiments

### Static inverse-bind controls

Two no-growth controls modified the inverse-bind relationship for slot 3 in LODs 0–3:

| Variant | Requested world displacement | Main SHA-256 | Observed result |
| --- | ---: | --- | --- |
| `offline_control` | 5.0 m | `4FB091EBC22A79F7A75AF1931B290B0061FB88874045844460E199CD06E50FD9` | Severe geometry extension across the scene in the manual screenshot |
| `offline_control_1p5m` | 1.5 m | `833643A5CB064A22A55B562FC144DD6C986C769014F5D06B8A8AE88E0065DDC1` | Created as a smaller control; the session did not preserve an equally clean automated comparison |

The magenta stretched geometry in the supplied screenshot is the strongest positive result so far. It came from the offline patch, not from an add-on active write. It proves that the edited patch-level inverse-bind data can affect rendered geometry. The magenta appearance may also include a material/shader fallback, so it does not by itself identify an anatomical bone or prove a clean skeletal translation.

### Grown marker plus vertex reweight

The first grown patch:

- Grew all six palettes by 64 slots: `88→152`, `88→152`, `88→152`, `88→152`, `2→66`, `4→68`.
- Pointed the added slots at scene-graph node 11.
- Copied a donor inverse bind into the new tail.
- Rewrote 9,596 vertex bone-index bytes from slot 3 to new slot 88.
- Passed 236 offline structural assertions.

Its main patch hash was `F66BD35B3AF01B7A5E1A4851BFB0453200716CC3F75B44621DAF423F6CA8F731`; its reweighted GPU-resource hash was `FF66C563909E9B5400ADB92E89A71B33E852998DBEE90DD48663A3A6571E766F`.

The game repeatedly failed or TDR-crashed with this family of reweighted patches. This invalidates the assumption that parser consistency and addressability checks prove game-level correctness. The exact semantic error in the reweighting has not been isolated.

### In-place fingerprint variants

Two size-preserving attempts reused existing slots:

- A 64-slot run duplicated donor slot 3 into slots 15–78 in LODs 0–3 and reweighted 9,596 vertex index bytes. It crashed/TDR'd before a useful runtime edit result.
- A one-slot variant duplicated slot 3 into slot 15 and performed the same reweighting. It also crashed/TDR'd.

These variants show that physical palette growth was not the only possible cause. Reweighting, an incorrect remap assumption, or downstream shader expectations remain more likely suspects.

### Marker-only growth control

The marker-only patch used the same 64-slot growth but did not reweight any vertices:

- Added slots 88–151 to the four 88-slot palettes, 2–65 to the 2-slot palette, and 4–67 to the 4-slot palette.
- The added tail points to node 11 (`0x37EF0820`).
- For the main four LODs, every added inverse bind is a copy of slot 3.
- GPU and stream files are byte-identical to the original.
- It passed 228 offline structural assertions.

Run `20260914-002121` completed approximately 85 seconds of startup, walking, turning, and the `B` stretch path without a crash. This is good evidence that palette growth and a repeated unused tail are tolerated. It does not prove that the new slots affect vertices, because no vertices reference them.

## Runtime experiment phases

### 1. Loader, console, scanner, input, and lifecycle

The first builds established add-on registration, a standalone console, heartbeat telemetry, and anonymous transform scanning. Manual movement and turning changed displayed raw values, including values resembling `0 → -180`. That was valid level-1 evidence, but it was incorrectly discussed as if it were likely bone motion. Camera, object, lighting, physics, and other transforms can produce the same pattern.

Input automation initially failed because the console and render window share the `helldivers2.exe` PID. Process focus was not enough; the exact render window needed focus. The runner was changed to minimize the console, focus/click the render window, delay startup input, hold `W`, add `D`, and tap `B` for the stretch emote.

The runner now checks PID and process start time before deployment, input, and shutdown. Recovery mode detects exited-but-still-present process objects and refuses unsafe per-thread termination. Steam itself is not managed.

### 2. Anonymous edit sweeps

Several builds selected transform-shaped mapped ranges and wrote increasingly drastic translations. The largest stable sweep alternated XYZ offsets of `±40`, `±80`, and `±120` across every slot in 49 candidates. Run `20260913-205746` recorded 9,569 successful writes and immediate readbacks, 29 later readbacks, 42 captures, and a clean shutdown. No player, NPC, or equipment deformation was visible.

This is strong negative evidence against the anonymous-candidate assumption. It does not prove that runtime palette editing is impossible. It proves that the candidates selected by that scanner were not the render-driving player palette at the time of the writes.

### 3. Generic upload-ring and capture iterations

The next builds tried to track transform runs and possible ring reuse in buffers already mapped by the game. Some runs wrote hundreds or thousands of matching ranges and observed later replacement. No visible deformation was established.

The generic scanner had two serious problems:

- It recognized numerical shapes without draw or mesh ownership.
- Earlier versions performed risky reading/mapping work on D3D12 upload resources and correlated with device-hung crashes.

The add-on stopped mapping resources itself and was restricted to ranges currently mapped by the game. A streaming SSE4.1 read helper was later added for write-combined memory. Those changes improved stability, but did not solve ownership.

External `CopyFromScreen` window captures correlated independently with D3D device-removal/TDR failures. Steam `F12` screenshots succeeded in later stable runs and are the current low-risk capture route. This is correlation, not a proven single crash cause.

### 4. Natural inverse-bind fingerprint

The original B-01 chest inverse-bind table supplied a 1,024-byte prefix fingerprint:

- Target slot: 3
- Needle: slot 7, 64 bytes
- Prefix FNV-1a: `0x4BA92B1D2AEA26F5`
- The prefix hash matched LODs 0–3 in the source patch.

The add-on found one writable process-memory occurrence after scanning roughly 3.7–3.9 GB. A one-shot 1.5 m write read back successfully. In later runs the engine replaced the bytes, the add-on rewrote them, and restoration succeeded.

Steam screenshots from run `20260914-001002` showed no conspicuous before/active/restored change. The discovered occurrence was therefore a CPU-private or otherwise non-consumed copy for the captured draw. The natural fingerprint correctly identified matching data, but not the active upload copy.

### 5. Marker-tail upload-ring discovery

With the marker-only patch installed, the scanner found hundreds and then 1,024 capped occurrences of 64 identical packed 48-byte transforms in mapped buffers. Some addresses changed over time, confirming upload-ring reuse.

Run `20260914-002902` wrote 64 selected copies once and read the same bytes back millions of times. None was overwritten during the edit interval and no visible change occurred. Those were stale copies. The runner nevertheless labeled the run `passed-edit-channel`; that status is a false positive for the actual goal.

A draw-cursor experiment then counted draw calls with index counts 80,586 and 23,526. Run `20260914-003749` saw 1,488 such focus draws and 11 reused marker addresses, but located zero live positions through the forward-window prediction. Its one selected copy was again not overwritten and produced no visible change.

The last two attempts used bounded residency probes to identify copies that the engine would refill during matching draws:

- Run `20260914-004621` selected changed addresses too late. By activation, the candidates had been recycled and no longer contained an intact marker, so zero probes armed.
- Run `20260914-004901` corrected that selection error and chose eight intact candidates, but none showed a verified engine restore during the relevant focus draws. No write was issued and the run ended `failed-edit` without crashing.

This is the current stopping point.

## Complete automated run ledger

Legend: `C` = anonymous candidates, `M` = moving anonymous candidates, `R` = marker/ring targets, `RM` = moving/reused ring targets, `W` = successful writes, `PR` = later/present readbacks. Runner labels are reproduced exactly even when misleading.

### Loader and automation establishment

| Run | Runner result | Recorded facts |
| --- | --- | --- |
| `20260913-170131` | `passed-no-motion-yet` | Add-on registered; frame 3,208; 134 draws; C=1; no input/edit telemetry in this early schema. |
| `20260913-170813` | `failed-runtime` | Same initial build; add-on registered; process still present; no usable runtime telemetry at report time. |
| `20260913-172708` | `failed-runtime` | Add-on registered; process ended before input; C=0. |
| `20260913-173033` | `failed-runtime` | Process ended; frame 17,744; 1,947 draws; C=64; input not completed. |
| `20260913-174312` | `passed-no-motion-yet` | Automation stage 8; input completed; three captures; C=7; clean exact-PID shutdown. |
| `20260913-174555` | `passed-no-motion-yet` | Stage 8; input completed; three captures; C=4; clean shutdown. |
| `20260913-174815` | `passed-no-motion-yet` | Stage 8; input completed; three captures; C=8; clean shutdown. |
| `20260913-175125` | `failed-input` | Stopped at stage 3; input never began; one start capture; C=12/M=0; game remained alive and was shut down. |
| `20260913-180550` | `failed-input` | Stopped at stage 3; input never began; one start capture; C=7/M=0; clean shutdown. |
| `20260913-180954` | `failed-runtime` | Reached stage 7 and emitted the walk/turn/stretch sequence, but died before completion; two captures; C=3/M=0; 1,179 draws. |
| `20260913-181357` | `failed-runtime` | Three early intro attempts; stopped at stage 3; process died; Windows later reported `0xc000000d`/`0xc0000026` application failures. |

### Anonymous write/readback experiments

| Run | Runner result | Recorded facts |
| --- | --- | --- |
| `20260913-184754` | `failed-shutdown` | Input/edit completed; W=118,393, PR=27; C=64/M=58; eight captures; process briefly remained half-terminated. |
| `20260913-185144` | `passed-edit-channel` | W=178,616, PR=3; C=64/M=57; overwrite and restore observed; eight captures; no unambiguous skeletal deformation. |
| `20260913-190521` | `failed-runtime` | Died at stage 3 before walking/edit; C=1/M=0; two captures. |
| `20260913-191544` | `passed-edit-channel` | W=1,694,051, PR=23; C=64/M=55; 34 captures; clean shutdown; no verified rendered edit. |
| `20260913-192430` | `passed-edit-channel` | Drastic 47-candidate sweep; W=367,495, PR=6; C=64/M=60; 21 captures; no verified deformation. |
| `20260913-192926` | `failed-runtime` | Died at stage 3 before edit; C=7/M=1; half-terminated process state. |
| `20260913-193114` | `failed-runtime` | Died at stage 3 before edit; C=7/M=0; half-terminated process state. |
| `20260913-193427` | `failed-runtime` | Died at stage 3 before edit; C=8/M=0; shutdown check later cleared. |
| `20260913-202032` | `failed-input` | Stayed alive at stage 3; no edit; C=12/M=0; clean shutdown. |
| `20260913-202524` | `failed-input` | Stayed alive at stage 3; no edit; C=7/M=0; clean shutdown. |
| `20260913-203011` | `failed-runtime` | Reached stage 5, then died before edit; three captures; C=9/M=0. |
| `20260913-203322` | `failed-runtime` | Died at stage 2; one capture; C=3/M=2; no edit. |
| `20260913-204351` | `failed-edit` | Passive scanner; 41 rounds; W=7,372, PR=30; 35 captures; one target unmapped before exact restoration; no visible deformation. |
| `20260913-205746` | `failed-edit` | Exaggerated 49-round `±40/±80/±120` sweep; W=9,569, PR=29; 42 captures; one transient unmap; no visible deformation. |

### Generic ring targeting, capture isolation, and crash cluster

| Run | Runner result | Recorded facts |
| --- | --- | --- |
| `20260913-212212` | `failed-runtime` | Died at stage 3 before edit; C=6/M=0; R=237/RM=0; two captures. |
| `20260913-212402` | `failed-runtime` | Died at stage 3; C=2/M=0; R=3/RM=0; two captures. |
| `20260913-213221` | `inconclusive-load` | No add-on registration in the captured log interval; process ended; two captures. |
| `20260913-214027` | `passed-edit-channel` | Selected 128, wrote 127, restored 123; W/PR=127/127; R capped at 1,024; eight captures; no visual proof. |
| `20260913-214524` | `failed-runtime` | Died at stage 3 before edit; C=2; two captures. |
| `20260913-214918` | `passed-edit-channel` | Selected/wrote/restored 128/128/128; W=21,632, PR=21,504; overwrite seen; no captures; no visual proof. |
| `20260913-215559` | `failed-runtime` | Died at stage 3; C=4; R=9; no edit. |
| `20260913-220239` | `passed-edit-channel` | Selected/wrote/restored 127/127/127; W=19,431, PR=19,304; three captures; no visual proof. |
| `20260913-220709` | `failed-runtime` | Died at stage 3; C=2/M=1; no edit. |
| `20260913-220831` | `passed-edit-channel` | Selected/wrote/restored 16/16/16; W=3,216, PR=3,200; three captures; no visual proof. |
| `20260913-222127` | `passed-edit-channel` | Selected/wrote/restored 32/20/20; W=3,240, PR=3,220; no captures; no visual proof. |
| `20260913-222344` | `failed-input` | Reached stage 13 but automation error 1; no write; C=64/M=63; R=1,024; clean shutdown. |
| `20260913-222759` | `failed-runtime` | Heartbeat remained at frame 1/stage 0; process stayed alive and was shut down. |
| `20260913-223055` | `failed-runtime` | Same frame-1/stage-0 stall; process stayed alive and was shut down. |
| `20260913-223431` | `passed-edit-channel` | Selected/wrote/restored 32/32/19; W=4,803, PR=4,771; no captures; no visual proof. |
| `20260913-223711` | `passed-edit-channel` | Selected/wrote/restored 32/32/32; W=5,712, PR=5,680; four captures; no visual proof. |
| `20260913-223950` | `failed-runtime` | Frame-1/stage-0 stall; process stayed alive and was shut down. |
| `20260913-224040` | `passed-edit-channel` | Selected/wrote/restored 32/29/2; W=918, PR=889; four captures; runner still marked pass because any restore set the Boolean. |
| `20260913-224512` | `failed-edit` | Window-capture run; selected 32 but wrote 0; edit error 2; four captures; game stayed alive. |
| `20260913-224841` | `failed-runtime` | Died at stage 3; C=8/M=1; R=20; no edit. |
| `20260913-224952` | `failed-runtime` | Frame-1/stage-0 stall; process stayed alive and was shut down. |
| `20260913-225130` | `failed-edit` | Window-capture run; no valid selected target and no write; edit error 2; four captures; clean shutdown. |
| `20260913-225518` | `failed-runtime` | Stalled at stage 1 with frame 3,370 and zero last-frame draws; process stayed alive and was shut down. |
| `20260913-225806` | `failed-runtime` | Died at stage 3; C=2; no edit; half-terminated process state. |
| `20260913-230031` | `failed-runtime` | Died at stage 3; C=4; R=9; no edit; half-terminated process state. |

### Offline controls, patch variants, and natural fingerprint

Patch association was not written into the run summaries. Associations in this subsection come from generation timestamps and session notes; where uncertain, the ledger states only the recorded runtime facts.

| Run | Runner result | Recorded facts |
| --- | --- | --- |
| `20260913-231509` | `failed-input` | Reached stage 7; movement sequence did not complete by runner criteria; C=1/M=1; R=3; no edit; clean shutdown. |
| `20260913-231745` | `passed-with-motion` | Input completed; C=64/M=57; R=1,024; ready/walk/stretch window captures; this period included the offline static-control work. The status only reflects anonymous memory motion. |
| `20260913-233914` | `failed-runtime` | Natural process-memory hunt scanned 2.98 GB and found 82 broad hits in that build; process died at stage 3; one window capture. |
| `20260913-234238` | `failed-runtime` | Frame-1/stage-0 stall; no scan; process stayed alive and was shut down. |
| `20260913-234800` | `failed-runtime` | Died at stage 3; C=8/M=1; one window capture; no edit. |
| `20260913-235311` | `failed-runtime` | Died at stage 3; C=7/M=0; one window capture; no edit. |
| `20260913-235552` | `failed-runtime` | Died at stage 3; C=11; R=11; one window capture; no edit. |
| `20260913-235815` | `failed-runtime` | Frame-1/stage-0 stall; no buffers or scan; process stayed alive and was shut down. |
| `20260913-235923` | `failed-runtime` | Died at stage 3 after 3,240 frames; one window capture; no edit. |
| `20260914-000109` | `failed-runtime` | Frame-1/stage-0 stall; process stayed alive and was shut down. |
| `20260914-000212` | `failed-runtime` | Exact natural fingerprint: one hit after 3.77 GB; one write and immediate readback; no later readback; engine replacement prevented exact restore; input completed; no capture. |
| `20260914-000541` | `passed-edit-channel` | One natural-fingerprint target; W=2, PR=673; overwrite observed; restored 1/1; input and stretch completed; no capture. CPU memory control only. |
| `20260914-001002` | `passed-edit-channel` | One natural-fingerprint target; W=2, PR=745; overwrite and restore observed; three Steam captures; no conspicuous rendered change. |

### Marker-only upload-ring tests

| Run | Runner result | Recorded facts |
| --- | --- | --- |
| `20260914-002121` | `passed-no-motion-yet` | Marker-only patch stability control; input/stretch completed; frame 5,496; 811 draws; clean shutdown; no edit requested. |
| `20260914-002902` | `passed-edit-channel` | R=838; selected/wrote/restored 64/64/64; W=64; PR=4,819,584; no overwrite; three Steam captures; no visible deformation. Selected copies were stale. |
| `20260914-003749` | `passed-edit-channel` | R=1,024/RM=11; 1,488 focus draws; zero predicted live locations; one stale write/readback/restore; no overwrite; three Steam captures; no visible deformation. |
| `20260914-004435` | `failed-runtime` | Process died at stage 1 before scan, input, selection, or write; zero hits and zero writes. Windows reported a BEX64/`0xc000000d` startup failure rather than a completed mutation test. |
| `20260914-004621` | `failed-input` | R=1,024/RM=9; late candidate selection found zero intact marker probes; no writes; automation error 4; one Steam before-capture; stable through shutdown. |
| `20260914-004901` | `failed-edit` | R=1,024/RM=6; eight candidates selected; none verified as freshly restored during focus draws; zero writes; three Steam captures; no crash; clean shutdown. |

## Aggregate runner results

Across 69 summaries:

- 34 `failed-runtime`
- 7 `failed-input`
- 5 `failed-edit`
- 1 `failed-shutdown`
- 1 `inconclusive-load`
- 15 `passed-edit-channel`
- 5 `passed-no-motion-yet`
- 1 `passed-with-motion`
- 25 runs had no live game process at the final pre-shutdown sample
- 44 runs recorded successful shutdown handling
- 51 requested an edit test; 22 completed the runner's edit state machine; 19 recorded at least one successful CPU-memory write

These counts are engineering history, not a success rate. Builds, patches, capture methods, and runner logic changed repeatedly, so the runs are not independent trials of one fixed configuration.

## Crash findings

There were at least two distinct failure families:

1. D3D device removal/TDR evidence, including `DXGI_ERROR_DEVICE_HUNG` (`0x887A0006`) and D3DDRED2 records, appeared during the risky scanner/capture/patch period.
2. BEX64/`ntdll` application failures with `0xc000000d` or `0xc0000026` also occurred, including startup failures before the test performed a scan or write.

The available summaries do not contain a WER event ID and active patch hash for every run. It is therefore not defensible to attribute all crashes to one cause. The evidence supports these narrower statements:

- Self-mapping or aggressively reading D3D12 upload resources was unsafe and was removed.
- Desktop/window `CopyFromScreen` capture correlated with additional TDR failures and was replaced by Steam screenshots.
- Reweighted offline patch variants repeatedly failed, while marker-only growth completed a stable 85-second run.
- Some startup crashes happened before any edit mutation, so “the write crashed the game” is not a valid explanation for every failure.
- The latest residency run (`20260914-004901`) made zero writes and shut down cleanly.

## Assumption audit

| Assumption | Evidence | Verdict |
| --- | --- | --- |
| A changing transform-like slot is probably a bone | Many anonymous candidates moved; drastic edits changed no character geometry | Rejected |
| Immediate readback proves render control | Millions of readbacks occurred without a visual change | Rejected |
| Engine overwrite proves the copy is consumed by the target draw | Natural-fingerprint memory was overwritten but Steam captures did not change | Insufficient |
| Any exact repeated marker tail is the live palette | Hundreds of stale copies were found; 64 edited copies had no effect | Rejected |
| Address reuse can be predicted with a simple forward stride/cursor | 1,488 focus draws produced zero predicted live locations | Rejected for current model |
| Index-count filtering identifies the character draw sufficiently | Focus counts were observed, but no matching live palette was verified | Insufficient |
| One physical palette serves all body parts because they share an armature | Unit resources own separate IB/remap/weight data and upload copies | Rejected |
| The offline patch can be avoided entirely | Anonymous and natural fingerprints did not provide stable draw ownership | Rejected for a dependable first product |
| Passing offline structural gates makes reweighting safe | 236 gates passed, but reweighted variants crashed | Rejected |
| Palette growth itself causes the crash | Marker-only growth ran stably | Not supported |
| Window capture is harmless | `CopyFromScreen` correlated with TDR failures; Steam capture was stable | Rejected for this test setup |
| `passed-edit-channel` means the visual edit worked | Multiple pass-labeled runs had no visible effect | Rejected; label is misleading |
| The magenta result came from active runtime writing | It came from the offline slot-3 control patch | Rejected |

## Runner and telemetry defects discovered during audit

- `passed-edit-channel` requires write/readback/restore, but no visual-difference criterion. It must be renamed or treated only as a memory-channel result.
- `passed-with-motion` means an anonymous candidate changed, not that the avatar visibly moved or that the candidate is a bone.
- `input_completed` means the key state machine finished. It does not prove the game acted on every key.
- The summary field `edit_refills_observed` is currently assigned from `edit_present_readbacks`. It is not an independent refill counter and must not be used as refill evidence.
- Some builds set the aggregate restore Boolean when any target restored, even if many selected targets did not. Run `20260913-224040` is an example: 32 selected, 29 written, only 2 restored, yet restore was Boolean true.
- Patch identity and installed patch hashes were not recorded per run. That prevents exact attribution for part of the crash cluster.
- Windows crash-event identifiers were inspected interactively but not archived per run.
- Capture method changed during the series and was not present in the earliest summary schema.

## What is proven and what remains unknown

### Proven

- The add-on can load and run callbacks in the game.
- The game exposes changing transform-shaped data in mapped D3D12 memory.
- Those allocations can often be written, read back, overwritten later by the engine, and sometimes restored.
- The B-01 patch contains inverse-bind/remap structures that can be parsed and modified consistently offline.
- A large static inverse-bind displacement affects rendered B-01 chest geometry.
- A 64-entry unused marker tail is accepted by the game and propagates into many upload-ring copies.
- Walking/turning and the `B` stretch sequence are useful automation stimuli when the render window has focus.

### Not proven

- Which runtime copy is consumed by a particular B-01 chest draw.
- A runtime before/active/restored visual deformation.
- A stable mapping from runtime slot to vertex set.
- A clean semantic mapping such as left shoulder or right thigh.
- Correct vertex reweighting into a newly added palette slot.
- A safe universal capture path other than the tested Steam screenshot route.
- That any single observed crash has one exclusive root cause.

## Current code state for review

The source snapshot accompanying this report is an experimental accumulation, not a clean release:

- Anonymous scanning is disabled by `k_enable_anonymous_scan = false`.
- B-01-specific constants assume a 64-entry marker tail and 152-slot main palettes.
- Natural process-memory fingerprint code remains alongside marker upload-ring code.
- Focus draw filters are hard-coded to index counts 80,586 and 23,526.
- Residency probing is capped at eight candidates.
- The active displacement constant is 1.5 m.
- Write-combined reads use an SSE4.1 streaming-load helper.
- Abandoned or unsuccessful locator paths still coexist and need simplification after review.

The current source is useful for diagnosis, but should not be described as a generic armature adjuster or distributed as a validated editor.

The exact snapshot was rebuilt before publication. The existing API-17 build was up to date and its isolated `addon_loads_and_registers` test passed. This verifies DLL loading and callback registration only; no additional game run was performed.

## Current installed state at report time

The process was checked before the report: no `helldivers2.exe` process was running.

The game directory contained the marker-only B-01 patch:

- Main: 80,904 bytes, SHA-256 `F66BD35B3AF01B7A5E1A4851BFB0453200716CC3F75B44621DAF423F6CA8F731`
- GPU resources: 5,173,272 bytes, SHA-256 `A4CB635C27FF4CA0056DF2A5AB9CAAEF7CABD3AAFC698038B01CE962CB10FD1F`
- Stream: 0 bytes, SHA-256 `E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855`

The installed add-on was 340,992 bytes with SHA-256 `EDF1668941D80854C2479B5A14E13121ED8D30E6736230F2F1C5030B72C98BDE`, matching run `20260914-004901`.

The private GitHub key file is ignored and was not read or added to Git.

## Questions that should be resolved before another live test

1. At exactly which callback and command-list point does the reference implementation locate and modify the fresh packed palette?
2. Does it identify the upload copy from a preceding `copy_buffer_region`, descriptor binding, root constant, or another draw-local relationship rather than from scanning history?
3. What exact byte layout and multiplication convention does the runtime 48-byte transform use at the write point?
4. Which material/LOD draw consumes the B-01 chest palette, and can that be tied to a resource plus offset without relying only on index count?
5. Why did the three reweight variants fail even though the offline parser gates passed? In particular, are mesh-local indices, real/fake remaps, palette limits, or material-specific streams being conflated?
6. Can the next test record the installed patch hashes, add-on hash, capture method, WER event, selected resource/offset, draw identifier, and visual comparison in one immutable result?

The next live experiment should not be another broad scan-and-write sweep. It should be a single-component, single-slot test whose target is derived from a verified draw-local upload event, with Steam before/active/restored captures and no external screen capture.
