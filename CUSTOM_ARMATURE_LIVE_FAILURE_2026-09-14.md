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
