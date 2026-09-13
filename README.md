# HD2 Armature Adjuster

The first-stage palette probe for a future HD2 armature-adjustment tool. This deliberately small ReShade add-on uses no mod, armature, VRM, Lua, or address cheat sheet. It scans CPU-visible D3D12 buffers for consecutive transform-shaped values and displays anonymous slots plus their frame-to-frame movement in its own console. Writes occur only during an explicitly requested, bounded edit test.

This version does **not** prove which draw or vertices consume a candidate. A `candidate / resource / offset / slot` is an observation, not yet a named bone. Descriptor-to-draw correlation and vertex-weight decoding belong in the next stage.

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

1. Use a 64-bit ReShade installation with full add-on support. This build requests API version 17 and is tested against ReShade 6.5.1.
2. Copy `HD2PaletteProbe.addon64` beside ReShade in the game's executable directory.
3. Launch the game. The probe opens a separate console after ReShade initializes a D3D12 device.
4. Move a visible character through animations and watch `max |delta|` for changing anonymous slots.

Keys are read while the game is running:

- `F6`: next candidate matrix run
- `F7`: next page of 16 slots
- `F8`: pause/resume discovery scanning (candidate sampling continues)
- `F9`: clear candidates and rescan from the start

The scanner reads at most one 64 KiB discovery window per presented frame, plus small snapshots of the selected and one rotating candidate. It never maps or writes GPU-only resources. Normal interactive operation is read-only.

## Automated live test

The add-on writes a one-second heartbeat to `%LOCALAPPDATA%\HD2ArmatureAdjuster\telemetry.json`. It contains counters and anonymous candidate statistics only.

After building, close the game and run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 `
  -GameRoot "F:\Steam\steamapps\common\Helldivers 2" `
  -ObserveSeconds 90 `
  -AutomateInput `
  -EditTest `
  -ShutdownAfterTest
```

The runner validates and deploys the DLL, launches the game through Steam if necessary, reads only the latest 600 ReShade log lines, watches telemetry, and saves a compact report under `test-results/<timestamp>/summary.json`. With `-AutomateInput`, the loaded add-on focuses ReShade's exact game window, lets startup settle for 25 seconds, and makes one short `W` intro attempt. Movement begins only after the configured delay and normal indexed scene rendering are both visible, so startup loading cannot consume the walking test. It then walks forward for three seconds, adds right movement for two seconds, and taps `B` for the stretch animation. Held keys are released if the exact game window loses foreground focus.

With `-EditTest`, the add-on waits briefly for a moving 48- or 64-byte candidate, adds `0.35` to one translation component for two seconds, verifies every write by readback, captures the active state, restores the exact original element, and records whether the game later overwrites that allocation normally. The additional captures are `capture-edit-before.bmp`, `capture-edit-active.bmp`, and `capture-edit-restored.bmp`. This validates the memory channel; only a clear visual change validates render ownership.

All captures come directly from ReShade's back buffer and are downscaled to 480 pixels wide. `-ShutdownAfterTest` terminates only the exact tested process after evidence collection; omit it to leave the game open.

Run `tools/run_live_test.ps1 -RecoverOnly` after a crash. It checks live and half-terminated `helldivers2.exe` entries, attempts ordinary exact-PID cleanup, and verifies that the installed add-on is unlocked. If Windows retains an already-exited process after cleanup, the procedure refuses deployment and asks for a Windows restart instead of using unsafe thread termination.

Synthetic input may be ignored by the game or its anti-cheat. The runner waits for normal scene rendering and reports `failed-input` instead of claiming a walking pass when the opening movie remains active.

Use `-PreflightOnly` to validate the paths, ReShade installation, DLL architecture, hash, and deployment without launching the game.

Possible report states:

- `passed-with-motion`: loading, callbacks, scanning, and moving candidates were observed.
- `passed-no-motion-yet`: the runtime works, but the observation window did not catch candidate motion.
- `passed-edit-channel`: bounded writes, readback, and restoration succeeded. This does not by itself mean the edited candidate affected the character.
- `failed-load`, `failed-runtime`, `failed-input`, `failed-edit`, `failed-shutdown`, or `inconclusive-load`: inspect the adjacent `reshade-tail.log` and summary fields.

`runtime_active` requires a heartbeat no more than five seconds old. The report also records whether the game process is alive and whether ReShade's last add-on event was registration or unregistration, so a frozen or detached runtime is not reported as a pass.

## What a useful result looks like

Prefer candidates with a high score, repeated `motion frames`, and multiple slots changing coherently when the character animates. Static transforms and unrelated transform arrays can also match, so this console is evidence for locating palettes, not final identification.
