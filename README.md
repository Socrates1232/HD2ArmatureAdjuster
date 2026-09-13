# HD2 Armature Adjuster

> **Current stage:** the add-on is a read-only, B-01-specific scanner for the loader-converted 48-byte inverse-bind table. It has not yet produced a verified runtime match or visible runtime deformation. Read [`REVIEW_RESPONSE_2026-09-14.md`](REVIEW_RESPONSE_2026-09-14.md) before running it; [`TEST_REPORT_2026-09-14.md`](TEST_REPORT_2026-09-14.md) preserves the earlier 69-run ledger.

The current validation build searches process memory for an exact offline-generated A/B profile of B-01's converted inverse binds. Its experiment mode is explicit in the console and telemetry. The current mode does not write game memory.

The older generic mapped-buffer and upload-ring experiments remain in the history, but they are not the current acceptance path. A full converted-table match is the next evidence gate.

## Build

The build uses the official ReShade 6.5.1 headers at API version 17, matching the runtime currently installed with the game. The pinned SDK revision is `f1332dfe8fb1c61a726d53af069cc2a2fcacae7f`.

Prepare the build-only dependency from the repository root:

```powershell
git clone --depth 1 --branch v6.5.1 --filter=blob:none --sparse https://github.com/crosire/reshade.git deps/reshade
git -C deps/reshade sparse-checkout set --no-cone /include/ /LICENSE.md
```

Then build with a 64-bit Visual Studio generator:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The add-on is produced at `build/bin/Release/HD2PaletteProbe.addon64`.

## Run

Use a 64-bit ReShade installation with full add-on support. This build requests API version 17 and is tested against ReShade 6.5.1. Copy `HD2PaletteProbe.addon64` beside ReShade in the game's executable directory and launch the game. The separate console reports full A/B converted-table hits and partial-anchor diagnostics. `F9` requests another scan after the current worker has finished.

The scanner excludes mapped graphics buffers, its own module, and its scratch storage. A hit is accepted only when all 88 entries equal profile A or profile B.

## Automated live test

The add-on writes a one-second heartbeat to `%LOCALAPPDATA%\HD2ArmatureAdjuster\telemetry.json`.

Before launch, the runner requires the installed B-01 patch triplet to match exact profile A or B. The current marker-expanded patch intentionally fails this gate. After installing a controlled triplet and building, run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 `
  -GameRoot "F:\Steam\steamapps\common\Helldivers 2" `
  -ObserveSeconds 90
```

The runner validates and deploys the newest DLL, launches through Steam if necessary, reads only the latest 600 ReShade log lines, watches telemetry, and saves `test-results/<timestamp>/summary.json`. A pass requires fresh telemetry from the exact process, `experiment_mode == converted_ib_scan`, and at least one full converted-table match. Anonymous motion and CPU write/readback are not pass conditions.

`-EditTest`, add-on capture, and direct window capture are disabled for this crash-sensitive read-only stage. With `-AutomateInput -SteamCapture`, the runner uses Steam's screenshot key for named load, ready, walk, and stretch states. `-ShutdownAfterTest` terminates only the exact tested process; omit it to leave the game open.

Run `tools/run_live_test.ps1 -RecoverOnly` after a crash. It checks live and half-terminated `helldivers2.exe` entries, attempts ordinary exact-PID cleanup, and verifies that the installed add-on is unlocked. If an already-exited process remains, `tools/recover_game_elevated.ps1` can repeat exact-PID cleanup through a UAC prompt. Both procedures refuse PID reuse and never manage Steam. If Windows still retains the process, restart Windows instead of terminating individual threads.

Synthetic input may be ignored by the game or its anti-cheat. The runner waits for normal scene rendering and reports `failed-input` instead of claiming a walking pass when the opening movie remains active.

Use `-PreflightOnly` to validate the paths, exact A/B patch state, ReShade installation, DLL architecture, hash, and deployment without launching the game.

The positive report state is `passed-converted-scan`. A clean run with no exact match reports `failed-no-converted-match`; inspect the partial-candidate fields before changing the search.

`runtime_active` requires a heartbeat no more than five seconds old. The report also records whether the game process is alive and whether ReShade's last add-on event was registration or unregistration, so a frozen or detached runtime is not reported as a pass.

## What a useful result looks like

An exact profile A or B hit establishes that a converted representation exists at the reported allocation. It does not yet prove that the active rendered instance consumes that allocation. The following stage must switch the known weighted slot between exact A and B bytes and obtain a controlled visual result.
