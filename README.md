# HD2 Armature Adjuster

The first-stage palette probe for a future HD2 armature-adjustment tool. This deliberately small, read-only ReShade add-on uses no mod, armature, VRM, Lua, or address cheat sheet. It scans CPU-visible D3D12 buffers for consecutive transform-shaped values and displays anonymous slots plus their frame-to-frame movement in its own console.

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

The scanner reads at most one 64 KiB discovery window per presented frame, plus small snapshots of the selected and one rotating candidate. It never maps or writes GPU-only resources and never writes into a game buffer.

## What a useful result looks like

Prefer candidates with a high score, repeated `motion frames`, and multiple slots changing coherently when the character animates. Static transforms and unrelated transform arrays can also match, so this console is evidence for locating palettes, not final identification.
