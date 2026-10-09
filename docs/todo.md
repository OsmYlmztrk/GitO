# MyGitGUI (GitO) to-do list

Agreed with the user on 2026-10-10. Keep this file current: tick items when done, add new decisions with a date.

## ⚠️ NEXT UP: VERY IMPORTANT (user, 2026-10-10)
- [x] **Ship the prebuilt GitBlueprintMerge plugin with GitO** (done 2026-10-10, waiting for the user's test).
  - Binaries are committed in `unreal/prebuilt/5.8/GitBlueprintMerge/` (dll, `.modules`, `.uplugin`, `LICENSE`; no Source/pdb). `build-plugin.ps1` refreshes that folder; commit it after every plugin change.
  - The csproj copies them next to `GitO.exe` as `UnrealPlugins/5.8/...` (build, publish, Velopack package; checked in a local `vpk pack`).
  - `Models/UnrealPluginInstaller.cs` copies them on startup to `<config dir>/UnrealPlugins/<engine>/GitBlueprintMerge/` unless the installed DLL is newer (a local dev build). Copy is atomic per file; a DLL locked by a running Unreal is skipped until the next start. Portable mode uses the same `ConfigDir`.
  - Hint text in `Views/Conflict.axaml` no longer mentions `build-plugin.ps1`.
  - Limits: UE 5.8 Launcher installs only (the DLL carries the engine `BuildId`); other versions need their own build.
- [ ] **User test:** install `GitO-win-Setup.exe` on a PC without the plugin (or after deleting `%APPDATA%\MyGitGUI\UnrealPlugins`), start GitO, check the folder is created; conflicted `.uasset` → "Merge in Unreal" opens our merge window.

## Waiting (user said "wait")
- Commit the uncommitted UI work: unpushed/unpulled commit badge (`Views/CommitStatusIndicator.cs`), narrower toolbar (`Views/RepositoryToolbar.axaml`), `Views/WorkingCopy.axaml`. Needs the user's test first.

## 1. Unreal merge: no silent losses (top priority)
Background: the plugin snapshots each revision into facts and applies theirs onto a copy of ours. Anything not in the snapshot keeps ours without telling the user. See `docs/unreal-merge.md` ("Still not compared (silent)").

- [ ] **Phase 1, detection.** Snapshot these as `Unmergeable` facts so they show as red "can't merge automatically" rows (option: leave out = keep ours), like the WBP widget tree:
  - timelines (`Blueprint->Timelines`, templates and curves)
  - inherited component override template contents
  - function local variables (`UK2Node_FunctionEntry`)
- [ ] **Manual apply for one-sided changes (user request).** Non-conflicting theirs changes are no longer applied automatically. Every change starts as "pending"; the user merges it element by element and sees the result in the Result pane. A "Merge all non-conflicting" button takes them all at once. Applies to all kinds (BP, WBP, ABP, DA, DT, MI).
- [ ] **Phase 2, real merge**, in this order, each with a MergeLab fixture + git scenario:
  1. function local variables (add/remove/default, like member variables)
  2. inherited component overrides (reuse component property merge on the override template)
  3. timelines (per track and keyframe; same track changed on both sides = conflict)

## 2. Conflict Radar phase 3
- [ ] **Windows notification** when GitO is minimized or in the background. Click opens GitO on the radar tab.
- [ ] **Taskbar overlay badge** with the active risk count (`ITaskbarList3.SetOverlayIcon`):
  - count = sum of active risks over all open repo tabs
  - 16x16 circle; colour by highest level: High `#E5484D`, Medium `#F5A524`, Low only `#8B8D98`
  - number in Segoe UI Bold 10 px `#FFFFFF`; above 9 shows "9+"
  - hidden while snoozed; hidden when the count is 0
- Not doing: pre-push warning, branch watch rules.

## 3. QoL
- [ ] **Long operation progress:** taskbar progress bar during LFS pull/push/fetch/clone, Windows notification when done ("Pull completed"). Shares the notification code with the radar.

## 4. Distribution and auto-update (decided 2026-10-10)
Decision: public GitHub repo, releases on GitHub Releases, installer + self-update with Velopack (MIT, delta updates, per-user install, no admin). Rejected hosts: Google Drive (virus-scan interstitial on files over 100 MB breaks automatic download, download quotas), private repo with token in the app.

- [x] **Bug first:** `App.Check4Update` read SourceGit's `version.json`. Fixed: it now uses Velopack (below); the temporary `DisableUpdateDetection=true` default is removed again.
- [x] Before going public: scan the whole commit history for tokens, internal paths and private assets. `.mcp.json` stays uncommitted.
- [x] User creates the public GitHub repo (`OsmYlmztrk/GitO`) and pushes `mygitgui` (outward-facing, the user does it). Depends on "GitHub fork + origin remote" under Waiting.
- [x] `VelopackApp.Build().Run()` at the top of `App.Main` (Velopack 1.2.161). Checked under NativeAOT: the install hook runs and exits.
- [x] `Check4Update` uses `Models/AppUpdater.cs` (Velopack `UpdateManager` + `GithubSource`). Startup banner and "ignore this version" kept; the dialog has "Update and Restart" (download with progress, apply, restart) and "Release Page". The automatic check is silent in copies not installed by the setup (dev builds).
- [x] Versions are now three-part: `VERSION` = `1.0.0`, tags `vX.Y.Z`; About and the update dialog show `vX.Y.Z`.
- [x] Point the About window and `.csproj` URLs at our repo instead of sourcegit-scm (`https://github.com/OsmYlmztrk/GitO`).
- [x] `.github/workflows/release.yml`: on a `vX.Y.Z` tag, AOT publish, drop `SourceGit.exe`/pdb, check `GitO.exe` and the plugin are there, `vpk download` (previous release, for deltas), `vpk pack` (`GitO-win-Setup.exe`, full/delta nupkg, portable zip), `vpk upload github --publish`. Release notes = commit subjects since the previous tag.
- [x] Bundle the prebuilt Unreal plugin with the installer (see NEXT UP).
- [ ] **User test of self-update:** install v1.0.0 with the setup, publish v1.0.1 (tag), start GitO → banner → "Update and Restart" → GitO restarts on v1.0.1 (About shows it).
- Known limit: the exe is unsigned, so Windows SmartScreen warns on first run. No fully free fix (Azure Trusted Signing is about $10/month; SignPath's free plan is for open-source projects only, could apply once public).

### License compliance and attribution (checked 2026-10-10)
Decision: standalone GitHub repo (not the GitHub "Fork" button), with clear SourceGit attribution. Our code is MIT. Bundled third-party parts keep their own licenses (BSD-3, OFL 1.1, EPL 1.0, public domain); all allow redistribution if their notices ship with the app.

Already fine: `LICENSE` keeps the SourceGit copyright; Unreal plugin sources carry MIT headers and contain no engine code (only the engine install path in `build-plugin.ps1`); `depends/AvaloniaEdit/LICENSE` is present; no Fork branding or icons.

- [x] `LICENSE`: keep `Copyright (c) 2026 sourcegit`, add `Copyright (c) 2026 Osman Yılmaztürk` below it (owner confirmed 2026-10-10).
- [x] `README.md`: replace the SourceGit README (badges, download links, screenshots) with a GitO one. First line: `Based on [SourceGit](https://github.com/sourcegit-scm/sourcegit) (MIT)`.
- [x] `src/SourceGit.csproj`: `Company` and `Copyright` still say `sourcegit-scm` (shown in GitO.exe file properties). Change to `Osman Yılmaztürk` and keep "based on SourceGit" in `Description`.
- [x] About window: "Based on SourceGit (MIT License)" line (12 px, `Brush.FG2`) linking upstream.
- [x] About window: source and release-notes links point at the GitO repo, website button removed; README, `SelfUpdate` download link and csproj URLs updated.
- [x] `THIRD-PARTY-LICENSES.md` is incomplete and partly wrong:
  - missing: `CommunityToolkit.Mvvm` (MIT), Inter font from `Avalonia.Fonts.Inter` (SIL OFL 1.1), Velopack (MIT, once added)
  - wrong versions: says Avalonia 12.0.5 / DataGrid 12.0.1, the csproj uses 11.3.22 / 11.3.13 (upstream's mistake)
- [x] Installer: ship `LICENSE` and `THIRD-PARTY-LICENSES.md` next to `GitO.exe` (MIT and OFL require the notice in every copy). Done via `None` items in the csproj (build + publish output); the Velopack package picks them up from the publish folder.
- [x] Unreal plugin: header says "MyGitGUI contributors"; change to `Copyright (c) 2026 Osman Yılmaztürk. MIT License.` `build-plugin.ps1` copies `LICENSE` into the installed plugin; the release zip must include it too. Plugin binaries are for people with their own UE license, so no engine files go into the zip.
- [x] `.github/workflows/`: all 7 upstream workflows deleted before the first push; `release.yml` comes with the Velopack step. On upstream merges, keep them deleted.
- `src/App.ico` (commit 5d845101): generated by the user with ChatGPT (2026-10-10). OpenAI's terms assign output to the user, commercial use allowed on all plans, so it ships under our MIT license. Mention it in the README "Built with AI" section.
- [x] AI disclosure (user wants it stated): README section "Built with AI": code written with Claude Code (Anthropic), icon AI-generated, reviewed and tested by Osman Yılmaztürk. Not required by MIT; for transparency. Commits already carry `Co-Authored-By: Claude`.
- Accepted risk: the external tool and shell icons (`Resources/Images/ExternalToolIcons`, `ShellIcons`: JetBrains, VS Code, Sublime, etc.) are their owners' trademarks, not MIT. Upstream ships them as nominative use (they only identify the tool); we keep them the same way.

## Later (not now)
- Scene / level (`.umap`, `__ExternalActors__`) merge: discussed separately.
- File lists: name + dim folder (`#E6E6E6` 13 px name, `#7A7A7A` 12 px folder), icons per extension.
- Local Changes type filters: Code / Asset / Map / Config.
- Prompt to LFS-lock an unlocked `.uasset`/`.umap` when it changes.
- Unity: set up `UnityYAMLMerge` for `.unity`/`.prefab` in one click.

## Rejected
- Discard undo, commit message prefix from branch name, pre-commit large-file/LFS check, sparse checkout manager.
