# Patch Editing and Profile Tool

`tools/patch_profile_tool.py` is the offline front end for inspecting an HD2
patch, applying a validated inverse-bind translation, and generating the runtime
profile consumed by the add-on. It needs only the finished patch triplet. It
does not require a VRM, armature, semantic bone names, or cheat-sheet data.

The implemented edit is deliberately limited to the operation already proven
by the live add-on: a world-space translation of one numeric inverse-bind slot.

## Inspect a patch

```powershell
python tools/patch_profile_tool.py inspect `
  --patch "path\to\0123456789abcdef.patch_0" `
  --out "build\0123456789abcdef.inventory.json"
```

The JSON inventory lists each parseable unit ID and every LOD's numeric slot
count and inverse-bind table hash. Omit `--out` to print without saving.

## Edit a copied patch and generate its profile

```powershell
python tools/patch_profile_tool.py translate `
  --patch "path\to\source\0123456789abcdef.patch_0" `
  --out-patch "build\edited\0123456789abcdef.patch_0" `
  --unit fa269172bd08695b `
  --slot 7 `
  --translate 0.0 0.15 0.0
```

The output patch must be in another directory and retain the source patch's
`16hex.patch_N` basename. The archive identity cannot be renamed.
The command creates:

- the edited main patch and byte-identical `.gpu_resources` and `.stream`
  companions;
- `<out-patch>.hd2profile` and its reviewable `.json` manifest; and
- `<out-patch>.edit.json`, recording source/output hashes, changed LODs,
  before/after values, and verified edit invariants.

By default the slot is edited in every LOD where it exists. Reduced LODs that
do not contain the slot are reported and skipped. Repeat `--lod N` to select
specific LODs instead; an absent LOD or out-of-range slot then fails the build.

Existing outputs are never replaced unless `--force` is supplied. Generated
patches cannot be written directly into a Steam or Helldivers 2 directory. This
keeps patch construction separate from installation and live testing.

## Translation semantics

For world-space displacement `d` and source inverse bind `IB`, the editor uses:

```text
IB_new = T(d) * IB
```

With the file's row-vector matrix convention, only floats 12-14 change by
`d * IB.linear`. The editor compares the complete source and result and fails
unless every changed byte belongs to exactly those translation rows in the
selected slot and LODs. The patch size must remain unchanged.

Translations must be finite, nonzero, and no component may exceed 10 metres.
That is a corruption guard, not a recommended visual-edit range.

## Profile-only pipeline hook

If another patching tool already performs the edit, generate only the runtime
package:

```powershell
python tools/patch_profile_tool.py profile `
  --patch "$finalPatch" `
  --out "$profileDirectory\$([IO.Path]::GetFileName($finalPatch)).hd2profile"
```

Optional repeatable `--unit 16hexid` filters work the same way as the standalone
extractor. A nonzero process exit means the pipeline output must not be shipped.

## Intended workflow

```text
finished source patch triplet
        |
        +--> inspect --> choose numeric unit / slot
        |
        +--> translate --> verified copied patch triplet
                               |
                               +--> matching .hd2profile + manifest
                               +--> edit audit report
```

Install the generated patch through the normal mod workflow. Install its
`.hd2profile` under `bin\HD2ArmatureProfiles` and list it in
`active_profiles.txt`. Multiple active packages are unioned by the add-on at
load time.

### Batch a modular patch tree

```powershell
python tools\patch_profile_tool.py profile-tree `
  --root "C:\path\to\duplicated-mod" `
  --out-dir "C:\path\to\duplicated-mod\HD2ArmatureProfiles"
```

This recursively profiles every patch main without changing any patch bytes.
It writes all extractable profiles for audit, plus `active_profiles.txt` with
only the profiles that contribute at least one new exact runtime table. The
add-on then unions those active profiles. Two patches need only one active
fingerprint when their unit ID, table size, and inverse-bind bytes are all
identical; variants with different table bytes must both remain active.
