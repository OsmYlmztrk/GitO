# Unreal asset merge — reference for agents

Three-way merge of conflicted Unreal `.uasset` files, change by change, instead of Unreal's built-in "keep one side" merge. Two parts:
- **Plugin:** `unreal/GitBlueprintMerge` — C++, UE 5.8 editor plugin, MIT.
- **GUI integration:** in MyGitGUI under `src/` — C#, Avalonia.

Read this before touching either side. Status as of 2026-10-04, branch `mygitgui`.

## Hard rules
- **License:** never copy Unreal Engine source into the repo (EULA); only call engine APIs. `KennethBuijssen/MergeAssist` has no license, so don't reuse it. `SeanAblur/BPMerge` is MIT: ideas are fine, keep its notice if code is copied.
- **No GUI automation:** don't click in MyGitGUI or Unreal windows to test. Verify the plugin with commandlets (see Testing). For UI changes, build and give the user a short checklist.
- **Build from an ASCII path:** the repo lives under `OneDrive\Masaüstü`, and UBT breaks on that path. Always build with `unreal/build-plugin.ps1`, which copies to `%LOCALAPPDATA%\Temp\gbm-<ver>` first.
- **Read the report, not the exit code:** Unreal's exit code also counts unrelated project errors. `report.json` is the source of truth.
- **Merge window UI:** buttons only; keyboard shortcuts were removed on request, so don't re-add them. User-visible plugin strings are English.

## Build and install
```
powershell -ExecutionPolicy Bypass -File unreal\build-plugin.ps1 -EngineVersion 5.8
```
- Installs to `%APPDATA%\MyGitGUI\UnrealPlugins\5.8\GitBlueprintMerge\`. The GUI finds it there via `UnrealEngine.FindMergePlugin`.
- Engine: `C:\Program Files\Epic Games\UE_5.8`.
- To check a build, grep the output for `error|GitBlueprintMerge\\Source.*warning|Result:`. Engine headers emit many unrelated C4996 warnings.
- GUI build: `dotnet build src/SourceGit.csproj -c Debug`.

## End-to-end flow
1. The user clicks **Merge in Unreal** on a conflicted `.uasset`. This calls `ViewModels/Conflict.cs`, which opens the `ViewModels/UnrealMerge.cs` popup (steps, timer, Terminate).
2. `Commands/CheckoutConflictStages` writes Base/Ours/Theirs to a temp dir (`git checkout-index --stage=all --temp`; LFS is smudged).
3. `UnrealEditor-Cmd.exe <uproject> -run=GitBlueprintMerge ... -Report= -Output=` runs headless. If there are no conflicts and nothing unsupported, it writes `Merged.uasset`.
4. If there are conflicts, it runs `UnrealEditor.exe <uproject> -GitBlueprintMergeUI ...`. This hides the main frame and shows the Slate merge window; closing it writes the report and quits the editor.
5. The GUI copies `Merged` over the working file, stages it, sends a notification and calls `App.RequestAttention()`.
6. Fallback: no report means the plugin can't read the asset (e.g. an unsupported type). The GUI then opens Unreal's own merge window (`-diff theirs ours base result`).

## Supported asset kinds
`AssetMerge.cpp: GetAssetKind`. Anything else is rejected by `LoadRevision`, which triggers the fallback above.

| Kind | Merged per change | Reported as unsupported (user may "leave out" = keep ours) |
|---|---|---|
| Blueprint (`UBlueprint`, so WBP/ABP pass too) | nodes, links, pin defaults, member variables, function graphs (add/remove/rename), CDO properties, properties of components present on both sides | parent class, interfaces, non-function graphs (add/remove), component add/remove/move, node moved to another graph |
| Data Asset (`UDataAsset`) | every `CPF_Edit` property; plain structs split into fields | asset class change, instanced subobjects, properties holding instanced objects |
| Data Table | row add/remove, every row field | row struct change |
| Material Instance (`UMaterialInstanceConstant`) | parameter overrides (all `*ParameterValues` arrays), static switches, `Parent`, other editable props incl. `BasePropertyOverrides` fields | material layers, static component masks (`staticother`) |

- **Containers are one value each:** arrays, maps and sets compare as a whole. MI parameter arrays are the exception: they are keyed by `ParameterInfo`.
- **Known gap, data loss risk:** a WBP widget tree / widget animations and an ABP AnimGraph are not in the snapshot. Theirs' changes there are dropped silently. Fix by snapshotting them into `FSnapshot::Unmergeable` at minimum.

## Plugin architecture
Pipeline: **load 3 revisions → snapshot each into facts → plan (3-way per fact + grouping) → resolve choices → apply theirs onto a fresh copy of ours → compile/save → report**.

### Facts
`FSnapshot::Facts` is a `TMap<key, value>`. Keys use GUIDs/names that are stable across revisions:

```
parent | interface/<path> | graph/<guid>=name|kind | node/<guid>=class|graph, node/<guid>/{title,pos,comment,enabled}
pin/<node>:<in|out>:<pin> | link/<srcpin>|<dstpin> | var/<guid>=name|type|category|flags, var/<guid>/default
cdo/<prop> | component/<guid>=name|class|parent, component/<guid>/prop/<name>
class | prop/<A.B[1]> | rowstruct | row/<escaped row>, row/<row>/<field.path>
param/<ArrayProp>/<Name[@LayerN|@BlendN]> | switch/<Name...> | staticother | subobject/<path>
```

Other `FSnapshot` members:
- `Labels`: readable names, keyed by GUID (BP) or by fact key (data assets).
- `SingleLinkPins`, `LinkedInputPins`, `NodeVariables`: helpers for grouping.
- `Unmergeable`: compared but cannot be taken from theirs, so `IsUnsupported` returns true for them.

**Entities vs details:** `node/`, `var/`, `component/` and `row/` are entities. Keys under them (`node/X/pos`, `row/R/Field`) are owned by them (`OwnersOf`, `IsEntityKey`). When an entity is added or removed as a whole, its details are marked `bPartOfWhole` and skipped by apply and counters.

### Files
| File | Key symbols | Role |
|---|---|---|
| `BlueprintSnapshot.h/.cpp` | `TakeSnapshot(UBlueprint*)` | BP → facts |
| | `PlanMerge` → `FMergePlan{Decisions, NumGroups,...}`, `FDecision{Key, Resolution, Base/Ours/Theirs, Group, bPartOfWhole}` | 3-way rule per fact, then conflict grouping: removed-vs-edited entity, removed variable still used, same-name variables, removed graph still edited, single-link pin contested, nodes inserted into a contested link |
| | `ResolveConflicts`, `FinalizePlan` | apply group choices, recompute `bPartOfWhole` and counters |
| | `FindInsertions`, `TakeBoth` | "take both": chain both sides' nodes inserted into the same exec link |
| | `IsUnsupported`, `FindUnsupported`, `LeaveOutUnsupported` | what can't be taken from theirs yet |
| | `Describe`, `SummarizeGroup`, `DescribeSubject`, `DescribeValue`, `FNamer` | human-readable texts for the report and the window |
| `BlueprintMergeApply.h/.cpp` | `ApplyTheirs(plan, ours, theirs, problems)` | BP apply: variables → remove/replace nodes → function graphs → copy nodes (`FEdGraphUtilities` export/import, GUIDs kept) → node/pin edits → links → component props → compile → CDO props (after compile). Returns false with problems instead of saving a broken BP |
| `AssetMerge.h/.cpp` | `EAssetKind`, `GetAssetKind`, `AssetKindName` | asset kind dispatch |
| | `TakeDataSnapshot`, `TakeAssetSnapshot` | DA/DT/MI → facts; `TakeAssetSnapshot` picks BP or data |
| | `ApplyTheirsToData` | DA/DT/MI apply via text export → retarget → import. DT rows via `FDataTableEditorUtils`. MI: `SetParentEditorOnly`, edits param arrays directly, then `UpdateStaticPermutation(set)` + `PostEditChange` |
| | helpers `AddValues`, `FindValue`, `SplittableStruct`, `ParameterId` | property path walking (`A.B[1].C`), struct splitting (all fields `CPF_Edit`, no native text import/export), MI parameter keys |
| `MergeSession.h/.cpp` | `FMergeSession` | owns Base/Ours/Theirs `FRevision`s, snapshots, plan, preview, merge, report. Shared by the commandlet and the window |
| | `Load`, `UpdatePreview(choices)`, `Merge(choices, out, pkg, leaveOut)`, `WriteReport`, `Apply` (private: BP vs data) | |
| | `FRevision{TempFile, Package, Asset, Blueprint}`, `LoadRevision`, `RenameRevision`, `SaveRevision`, `ReleaseRevision` | revisions are copied to `Saved/Diff/GBM_<label>_<guid>.uasset` and loaded side by side. Results are renamed to the real package name before saving |
| `GitBlueprintMergeCommandlet.*` | `UGitBlueprintMergeCommandlet` | `-run=GitBlueprintMerge` |
| `SGitBlueprintMergeWindow.*` | `SGitBlueprintMergeWindow` | Slate window, described below |
| `GitBlueprintMergeModule.cpp` | `FGitBlueprintMergeModule` | `-GitBlueprintMergeUI` startup: hide the main frame, open the window, write the report on close, `QUIT_EDITOR` |
| `GitBlueprintMergeTestCaseCommandlet.*` | `UGitBlueprintMergeTestCaseCommandlet` | test fixtures and dump, see Testing |

Merge window (`SGitBlueprintMergeWindow`):
- **Left panel:** conflict cards (Keep ours / Take theirs / Take both) above a "Merged automatically" list. Both are always visible, with no tabs.
- **Columns:** Ours / (Base, toggled) / Result / Theirs. 4 layouts: Top, Side, Classic, Result.
- **Column content:** BP shows a read-only `SGraphEditor` focused on the change's nodes. Other assets show a value panel plus a read-only details view with the property highlighted.
- **Absent values:** shown via `AbsentText`, e.g. "Removed in ours (was in Base)" vs "Not in this version".
- **Result column:** the live preview (`FMergeSession::UpdatePreview`).

### Temp-package names (important when editing snapshot/apply)
The three revisions load under different temp package names:
- Snapshots normalize values with `FNormalizer`, which replaces the own package name with `<self>`.
- Apply rewrites theirs' references with `FRetarget` (theirs package → ours package).

Any new value copy must go through text export + `Retarget`, or references will point into theirs' temp package.

### Commandlet CLI
```
UnrealEditor-Cmd.exe <uproject> -run=GitBlueprintMerge -PLUGIN=<...>\GitBlueprintMerge.uplugin -EnablePlugins=GitBlueprintMerge
  -Base=<f> -Ours=<f> -Theirs=<f> -Report=<json> [-Output=<f> -PackageName=/Game/...]
  [-Prefer=ours|theirs | -Choices=<json {"0":"theirs","1":"both"}>] [-LeaveOutUnsupported]
  -unattended -nullrhi -nosplash -nopause -abslog=<log>
UnrealEditor.exe <uproject> -GitBlueprintMergeUI <same> -OursLabel= -TheirsLabel= -nopause
```

### Report (`report.json`, mirrored by `src/Models/BlueprintMergeReport.cs`)
- `asset`, `kind`, `canAutoMerge`, `written`
- `summary{ours,theirs,both,conflicts,resolved}`
- `conflicts[{group,text,ours,theirs,details[]}]`
- `changes[{key,resolution,text,base,ours,theirs,group,reason}]`
- `unsupported[]`, `leftOut[]`, `problems[]`

## GUI side (C#)
| File | Symbol | Role |
|---|---|---|
| `src/Models/UnrealEngine.cs` | `UnrealEngine` | `IsAssetFile`, `FindProject` (walks up to the `.uproject`), `FindEditor` (EngineAssociation → launcher manifest / registry / source-build GUID), `FindMergePlugin`, `GetPackageName` (`Content/A/B.uasset` → `/Game/A/B`, plugin content → `/<Plugin>/...`) |
| `src/Models/BlueprintMergeReport.cs` | `BlueprintMergeReport`, `...Summary`, `...Conflict`, `...Change` | report DTOs; JSON source-gen entry in `src/App.JsonCodeGen.cs` |
| `src/Commands/CheckoutConflictStages.cs` | `CheckoutConflictStages` | stage files of a conflicted path; null for missing stages |
| `src/Commands/QueryConflictFileState.cs` | `IsConflictedLFSPointer` | an LFS pointer with conflict markers counts as a binary conflict |
| `src/ViewModels/UnrealMerge.cs` | `UnrealMerge : Popup`, `Mode{Auto, Resolve, UnrealWindow}`, `UnrealMergeStep`, `UnrealMergeTarget`, `UnrealMergeOutcome` | the step-by-step popup; `RunPluginAsync` builds the command lines; `StageAsync` writes and stages the result |
| `src/ViewModels/Conflict.cs` | `IsUnrealAsset`, `HasMergePlugin`, `CanMergeInUnreal`, `CompareInUnrealAsync`, `MergeInUnrealAsync`, `ResolveInUnrealAsync`, `OpenUnrealMergeToolAsync`, `ShowUnrealOutcome` | Unreal card state and commands on the conflict panel |
| `src/Views/Conflict.axaml(.cs)` | — | "Unreal Editor" card: 3-step explanation, ⓘ flyout "What Merge in Unreal can combine", buttons, last-run status, change list. **Keep the flyout text in sync with the supported-kinds table above.** |
| `src/Views/UnrealMerge.axaml(.cs)` | — | popup view |
| `src/App.axaml.cs` | `App.RequestAttention()` | flash the taskbar and chime when the app is in the background |

## Testing (no GUI)
The test project is `C:\Users\osman\OneDrive\Belgeler\Unreal Projects\MergeLab\MergeLab.uproject` (git + LFS). Its log shows unrelated errors (`BP_Door` may contain conflict markers, GameFeatureData); ignore them.

```
-run=GitBlueprintMergeTestCase -DataFixtures -OutDir=<dir>       -> DA_/DT_/MI_Fixture_{Base,Ours,Theirs}.uasset (package /Game/GBMFixtures/<Name>)
-run=GitBlueprintMergeTestCase -BlueprintFixture -OutDir=<dir>   -> BP_Fixture_{Base,Ours,Theirs}.uasset
-run=GitBlueprintMergeTestCase -Dump=<file>                      -> logs "FACT key = value" for every fact
-run=GitBlueprintMergeTestCase -Base=<BP_Test> -OutDir= -PackageName=   (legacy BP_Test scenario)
```

Typical check:
1. Generate fixtures.
2. Run the merge with `-Prefer=theirs -Output=...`.
3. Grep the log for `LogGitBlueprintMerge`.
4. `-Dump` the output and compare the facts.

Each fixture has exactly one conflict group per area:
- BP: BeginPlay text, `Health` CDO, `Light` intensity, plus 16 clean changes.
- DA: `LoadFilter`.
- DT: row C, which ours removed and theirs edited.
- MI: `Roughness`.

Git scenarios in MergeLab:
- `bp/base` → `bp/ours` and `bp/theirs`. All 4 fixtures; `git merge bp/theirs` on `bp/ours` gives 4 conflicts.
- `data/*`: DA/DT/MI only.
- `door/*`: older BP_Door scenario.

LFS: extract stages with `git show :N:path | git lfs smudge path`.

## Extending

### New asset kind
1. Add it to `EAssetKind` and `GetAssetKind`.
2. Snapshot it in `TakeDataSnapshot`: reuse `AddObjectProperties` / `AddValues`, and put not-yet-mergeable facts in `Unmergeable`.
3. Add a branch to `ApplyTheirsToData`.
4. If it introduces a new entity prefix, add it to `OwnersOf` / `IsEntityKey` in `BlueprintSnapshot.cpp`.
5. Add readable subjects in `FNamer::Subject`, or set `Labels[key]`.
6. Add a fixture to the test commandlet.
7. Update the GUI flyout and this doc.

### New BP capability (e.g. adding components)
1. Make `IsUnsupported` return false for it.
2. Implement it in `ApplyTheirs` in the right order: before compile for structure, after compile for CDO values.
3. Extend `-BlueprintFixture`.

### Keyed container merge
Model elements as entities with a stable key, like MI params or DT rows. Arrays without keys must stay whole-value.

## Open work (priority order)
1. **WBP widget tree / ABP AnimGraph silently dropped** (see above). At least report them as `Unmergeable`.
2. Blueprint: component add/remove/move, non-function graph add/remove, interfaces, parent class, node moved between graphs.
3. Element-wise merge for arrays/maps/sets; instanced subobject contents; MI layers and masks; DT row order (theirs' new rows are appended).
4. More kinds: String Table, Curve Table, User Defined Enum/Struct, Blackboard, Behavior Tree, Material, levels.
5. Distribution:
   - The plugin is only built for UE 5.8, and building needs Visual Studio.
   - The plugin name still says "Blueprint".
   - `report.json` `kind` is unused by the GUI.
   - No CI or automated tests.
6. Housekeeping:
   - `.mcp.json` (local Unreal MCP config, `127.0.0.1:8000`) is intentionally uncommitted.
   - Nothing is pushed.
   - `Unreal Projects\MergeLabData` is a leftover test copy and can be deleted.
