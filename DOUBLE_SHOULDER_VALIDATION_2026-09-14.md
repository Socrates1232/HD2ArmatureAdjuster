# Double shoulder preset validation — 2026-09-14

## Result

The user confirmed the doubled static shoulder-narrowing preset as valid in
game. This result applies to the legacy runtime behavior rooted at `361928e`;
it does not validate the new pose-correct generator against a live pose source.

## Exact tested artifact

- File: `profiles/lacrima_static_shoulder_targets_2x.txt`
- SHA-256: `841B9A3FE73E104B8A590A7A17356AD5A96BE12E0292F06719BE88D309EF1D0B`
- Qualified targets: 183
- Units: 11
- Exact converted-IB table fingerprints: 20
- Duplicate `(unit, table fingerprint, slot)` keys: 0

The map was generated from the same final patch/profile tree as the preceding
preset. Its 183 qualified keys are identical to the previous map. Only the
three translation fields were multiplied by two.

## Taper

| Branch depth | Previous magnitude | Validated magnitude |
| --- | ---: | ---: |
| Shoulder root | 0.03 m | 0.06 m |
| Depth one | 0.02 m | 0.04 m |
| Depth two | 0.01 m | 0.02 m |
| Depth three and below | 0 m | 0 m |

The signs are asset-side dependent and are retained exactly in the preset.
Repeated slot numbers across rows are not duplicate targets: palette slots are
local to their unit and exact table variant.

## Reproduction

```powershell
python tools/patch_profile_tool.py shoulder-targets `
  --root "path\to\profiled-mod-copy" `
  --profile-dir "path\to\profiled-mod-copy\HD2ArmatureProfiles" `
  --out "profiles\lacrima_static_shoulder_targets_2x.txt" `
  --report "build\lacrima_static_shoulder_targets_2x.json" `
  --left-translate 0.06 0 0 `
  --right-translate -0.06 0 0 `
  --falloff-depth 3
```

The generated report is not required by the runtime. It is review evidence;
`shoulder_targets.txt` is the data file consumed by the legacy add-on.
