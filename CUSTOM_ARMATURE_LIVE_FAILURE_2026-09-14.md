# Custom Armature Stage 1: First Live-Test Failure

## Result

The first live deployment of the Stage 1 runtime failed its activation gate. The game launched normally and the runtime loaded the custom rig package, but the user reported that pressing F8 froze the game. `helldivers2.exe` was no longer running when the state was collected.

This is an observed failure report, not a root-cause claim.

## Exact deployed inputs

- Source branch head: `8b5fff5c78427776f2765be0e4e7961373bccad2`
- Runtime binary: `HD2PaletteProbe.addon64`
- Runtime size: 542,208 bytes
- Runtime SHA-256: `98A94AD55E0EDA5B5FA94BBE9944D8448BB21D03969389BDF6889C771E4A038C`
- Target rig: `target2.hd2rig.json`
- Target SHA-256: `B0ED53302844709770DE6A052E27E7D3FAC7F0BFF3CC811857958B550513C2C7`
- Bundled source: `source.hd2source.json`
- Source SHA-256: `80EFF04DF8D8A5690A1917EF5C0A61EB2972351035A2FF5111F109AC88B83372`
- Rig ID: `8b95a58f639b6c612b35323c9d94073f2d6f262d00c04e22c87548f69e8672f6`

The installed runtime hash was verified against the branch build artifact before launch.

## What loaded successfully

The last telemetry snapshot, written at `2026-09-14 22:19:12 +08:00`, reported:

```text
addon_version                 1.2
experiment_mode              custom_armature_ready
profile_files_loaded         19
profile_tables_loaded        27
profile_load_errors          0
custom_rig_configured        true
custom_rig_file              target2.hd2rig.json
custom_source_file           source.hd2source.json
converted_ib_hits            69
```

This confirms package parsing and ordinary IB discovery before activation. It does not confirm that a replacement plan was reached or published.

## Last state before the freeze

```text
custom_status                DISABLED
custom_desired_enabled       false
custom_plans_built           0
custom_publications          0
custom_restores              0
custom_palette_candidates    10
custom_pose_samples          10
custom_rejected_instances    160
custom_scan_calls_total      60
custom_scan_wall_ms_total    19136.5
custom_scan_wall_us_last     88
custom_scan_wall_us_max      3562622
shoulder_toggle_count        0
```

Runtime 1.2 had no custom intent counter. `shoulder_toggle_count` belongs to the inactive legacy shoulder controller and says nothing about the custom F8 transition. The final snapshot therefore cannot establish the last accepted custom intent from `custom_desired_enabled` alone.

## Diagnosis

Source inspection found the blocking path in runtime 1.2:

```text
on_present
  -> F8 toggles custom runtime
  -> scan_custom_pose_input
       -> scan and validate about 264 KiB against every rig table
  -> custom_runtime.service
```

This is not merely a correlation. The expensive scan and service were directly invoked by the ReShade presentation callback. Telemetry measured 19.1 seconds of aggregate scan wall time across 60 calls, with one call taking 3.56 seconds. Repeating that work on the render thread explains the visible freeze.

Runtime 1.3 removes both operations from `on_present`. F8 now publishes one atomic intent revision, enables or disables small mapped-window capture, and signals a below-normal-priority worker. The worker owns pose matching, plan construction, guarded publication, and restoration. Scan steps are reduced to a 16 KiB payload plus 8 KiB overlap, and the generic hunt-thread cleanup no longer waits for up to one second on the presentation callback.

Telemetry schema 9 records the requested intent/revision separately from the worker-accepted revision and exposes worker busy/cycle timing. The resource CSV contains the same worker cost fields.

## Review target

The next review should verify that `on_present` contains no custom scan, plan, publication, restoration, or blocking worker wait. The next live test should first establish responsiveness and intent acknowledgement before judging deformation.

Minimum required instrumentation for the next build:

```text
F8_EDGE
ENABLE_INTENT_PUBLISHED
ACTIVATION_WORK_QUEUED
ACTIVATION_WORK_STARTED
ACTIVATION_WORK_FINISHED
PLAN_BUILT
PUBLISH_STARTED
PUBLISH_FINISHED
```

Runtime 1.3 exposes the equivalent requested/accepted boundary through telemetry counters. Append-only transition logging remains a useful follow-up if another process-ending fault prevents the periodic snapshot from being written.

## Runtime 1.4 and 1.5 follow-up

Runtime 1.4 remained responsive after F8 and eliminated the false subject gate. The ordinary scanner found 76 exact IB instances, while the custom runtime reported `WAITING_FOR_LIVE_POSE` and `custom_rejected_instances = 0`. This confirms that repeated exact instances are no longer misclassified as ambiguous. That run found no qualifying live palette.

Runtime 1.5 moved the 32 ms capture throttle behind the minimum useful mapped-buffer size check. Its final telemetry reported:

```text
custom_status                WAITING_FOR_LIVE_POSE
custom_intent_revision       3
custom_worker_intent_revision 3
converted_ib_hits            75
custom_palette_candidates    2
custom_pose_samples          2
custom_plans_built           0
custom_publications          0
custom_rejected_instances    0
custom_scan_wall_us_max      350166
custom_worker_wall_us_max    350181
```

The capture correction therefore recovered qualifying pose samples without restoring presentation-thread matching, but the run did not reach plan construction or publication. The current live gate is required-pose/plan coverage, not IB-table discovery or subject ambiguity. The final ReShade log tail showed runtime destruction and add-on unregistration without an exception entry; it does not by itself establish why the game process ended.

## Runtime 1.6 correction

The exported rig has three affected structural shoulder variants distributed across 18 of its 27 tables. No single table contains all three variants. Runtime 1.5 nevertheless evaluated every affected bone in the complete rig for every table, so a valid per-table pose was rejected whenever it could not provide the parent of an edited variant absent from that table.

Runtime 1.6 restricts a table's required pose set to the ancestor paths of affected slots actually represented by that table. It also omits the nine table layouts with no affected slots from custom-runtime binding and capture. The discovery matcher, pose ownership checks, matrix conversion, and guarded publisher are unchanged. A regression test now proves that an edited branch absent from a table cannot prevent the represented branch from producing a plan.

The next live gate is therefore specific: a qualifying pose sample must increment `custom_plans_built`, followed by `custom_publications` and `APPLIED`. If it does not, the next diagnostic should expose the exact per-table `pose_error` rather than changing discovery again.

## Runtime 1.6 live result and runtime 1.7 correction

Runtime 1.6 remained responsive, accepted three F8 intent revisions, found 75 exact IB instances, and recovered one qualifying pose sample, but again produced zero plans and publications. Its final status was `WAITING_FOR_LIVE_POSE`, not `LIVE_LAYOUT_UNVERIFIED`. That distinction proves the planner did not reject the sample: the service path considered it stale before calling the planner.

The worker passed the presentation-frame value captured before scanning into `observe_window`, then passed a newer presentation frame into `service`. The observed scan took 117,504 microseconds, enough for the three-frame freshness window to expire inside the same worker cycle. Runtime 1.7 passes the same logical frame to both operations. The freshness rule remains unchanged between cycles, and the scanner, plan math, and publisher are untouched.
