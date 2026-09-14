# Runtime Profile Extraction Pipeline

## Purpose

The runtime add-on must not know a mod's patch bytes at compile time. The extraction step turns each finished `.patch_N` into a small, immutable runtime profile. The same add-on binary can then identify and edit any table described by the active profiles.

This is deliberately a post-build hook: first finish the patch, then extract its profile. If the main patch changes afterward, regenerate the profile.

`tools/patch_profile_tool.py profile` exposes the same generation function for
new patching pipelines. Its `translate` command also provides a complete,
verified edit-copy-profile operation; see [`PATCH_EDITING_TOOL.md`](PATCH_EDITING_TOOL.md).

## Pipeline contract

For each main patch produced by the patching pipeline, invoke:

```powershell
python tools/extract_runtime_profile.py `
  --patch "$finalPatch" `
  --out "$profileDirectory\$([IO.Path]::GetFileName($finalPatch)).hd2profile"
```

Success produces two files:

- `<patch basename>.hd2profile`: the binary consumed by the add-on.
- `<patch basename>.hd2profile.json`: hashes and table inventory for humans and build logs.

The process exits nonzero if the main bundle is invalid, a requested unit is absent, a requested unit's inverse-bind structure is invalid, a table has an unsupported size, or no table can be extracted. A broad all-unit extraction can record irrelevant unit resources it cannot parse under `skipped_units`; explicitly requested units fail instead of being skipped.

Optional repeatable unit filtering is available when the patch contains unrelated units:

```powershell
python tools/extract_runtime_profile.py `
  --patch "$finalPatch" `
  --unit fa269172bd08695b `
  --unit 0123456789abcdef `
  --out "$profilePath"
```

## What is extracted

The extractor reads resources of unit type `e0a48d0be9a7453f`. For each selected unit it:

1. follows the unit's BoneInfo pointer;
2. enumerates its LOD inverse-bind tables;
3. converts every 64-byte file matrix to the loader's 48-byte packed affine layout;
4. merges byte-identical LOD tables into one record with a 32-bit LOD mask; and
5. stores the records in deterministic `(unit ID, first LOD, entry count)` order.

It does not mutate the patch. It does not require a VRM, an armature file, semantic bone names, or a cheat sheet.

The JSON manifest includes:

- main, GPU-resource, and stream file sizes and SHA-256 values when present;
- output profile size and SHA-256;
- unit ID, LOD mask, first LOD, entry count, packed byte count, and packed-table hashes;
- any non-requested unit resources that were skipped.

Only the main patch basename and SHA-256 are embedded in the binary package. Companion hashes remain in the manifest because inverse-bind source data comes from the main patch.

## Binary format: `HD2IBP1`

All integers are little-endian.

### 64-byte package header

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 8 | `HD2IBP1\0` magic/version |
| `0x08` | 4 | header size, currently 64 |
| `0x0c` | 4 | record count |
| `0x10` | 4 | UTF-8 patch-basename byte count |
| `0x14` | 4 | reserved, zero |
| `0x18` | 32 | main-patch SHA-256 |
| `0x38` | 8 | FNV-1a-64 of every byte after the header |

The UTF-8 patch basename immediately follows the header, without a terminator.

### Repeated 40-byte table record

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 8 | unit ID |
| `0x08` | 4 | LOD bit mask |
| `0x0c` | 4 | entry count |
| `0x10` | 4 | anchor slot, currently zero |
| `0x14` | 4 | table byte count, exactly `entries * 48` |
| `0x18` | 8 | FNV-1a-64 of the table |
| `0x20` | 4 | first LOD represented by the record |
| `0x24` | 4 | reserved, zero |

The exact converted table bytes immediately follow each record header.

The runtime parser rejects unsupported magic, invalid bounds, invalid entry counts, checksum failures, truncated records, and trailing data.

## Runtime selection and composition

Profiles live beside the add-on under:

```text
Helldivers 2\bin\HD2ArmatureProfiles\
```

`active_profiles.txt` contains one profile filename per line. The runner creates this file without a byte-order mark. Names cannot contain directory separators and must end in `.hd2profile`.

Every listed package is loaded and flattened into one runtime registry. Therefore separate armor, helmet, cape, body, or other patch outputs can contribute separate profiles. Exact duplicate records are merged only when unit ID, entry count, and all converted table bytes agree; their LOD masks are combined. Distinct variants of the same unit remain separate so the resident version can match. The add-on does not assume that one patch describes the whole character or that all body parts share one runtime table. Scan results and edit targets are also physical-address-deduplicated.

If no active-list file exists, all profile files in the directory are loaded. This is useful for manual use but less reproducible than an explicit list.

## Runtime identification

The scanner builds a key index from the first eight bytes of each converted table, then searches eligible writable private memory on four-byte boundaries. A candidate is accepted only when its entire `entries * 48` bytes equal the profile record.

The following are excluded:

- the add-on image;
- the profile data stored by the add-on itself;
- the scanner's scratch copy;
- currently mapped graphics-resource ranges; and
- guarded, inaccessible, and write-combined pages.

A first-slot match is diagnostic only. It is never sufficient for editing.

## Runtime edit semantics

An edit request supplies:

```text
unit ID + slot index + world-space translation (x, y, z)
```

For a source inverse-bind matrix `IB` and requested translation `T(d)`, the injected matrix is:

```text
IB' = T(d) * IB
```

In the game's packed row-vector layout, the translation row changes by `d * IB.linear`. The implementation decodes the 48-byte form, performs that operation, and re-encodes it. The conversion and affine identity are covered by native tests.

Editing is fail-closed:

1. require a complete exact profile match;
2. resolve the requested unit and validate the slot bound;
3. require current target bytes to still equal the profile source;
4. write and immediately read back the injected bytes;
5. rewrite only if a later present sees the exact source refill;
6. leave any third state untouched; and
7. restore only targets that still equal the injected bytes.

This profile identifies the table and supplies the static source matrix. It does not itself identify a human-readable bone name or prove vertex ownership. Those should be added as separate metadata once the offline vertex/remap reconstruction stage exists.

## Validation commands

Run all format, conversion, scanner, add-on-load, and synthetic extraction tests:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Validate profile-to-installed-patch identity and deploy without launching:

```powershell
powershell -ExecutionPolicy Bypass -File tools/run_live_test.ps1 -PreflightOnly
```

The runner refuses stale or mismatched installations under `-NoDeploy`, and refuses to replace the add-on, profiles, or active list while `helldivers2.exe` is running.
