# MyGitGUI

A personal, Windows-only Git GUI that looks like Fork, built on a fork of SourceGit (MIT, C# / Avalonia, .NET 10). It is used for Unreal/Unity game repos with Git LFS.

## Repo layout
- `src/`: the app (SourceGit code base).
  - Our changes in upstream files are marked with `// MyGitGUI:` comments.
  - Our new files say "MyGitGUI" in their summary.
- `unreal/GitBlueprintMerge/`: our Unreal Engine 5.8 editor plugin that three-way merges conflicted `.uasset` files (Blueprint, Data Asset, Data Table, Material Instance). **Read [docs/unreal-merge.md](docs/unreal-merge.md) before working on Unreal merge, on either the plugin or the GUI side.** User-facing setup and usage: [docs/unreal-guide.md](docs/unreal-guide.md); keep it in sync when Unreal features, supported asset kinds or UI texts change.
- `docs/todo.md`: the agreed to-do list (priorities, decisions, rejected ideas). Check it before proposing work; keep it current.
- `upstream` remote = sourcegit-scm/sourcegit. Work happens on branch `mygitgui`; `master` tracks `upstream/master`.

## Build
- App: `dotnet build src/SourceGit.csproj -c Debug`
- Unreal plugin: `powershell -ExecutionPolicy Bypass -File unreal\build-plugin.ps1 -EngineVersion 5.8`. Always use this script: it builds from an ASCII temp path, because UBT fails on the repo's `OneDrive\Masaüstü` path. It also refreshes `unreal/prebuilt/5.8/`, which GitO ships: commit that folder after plugin changes.
- Local Release (AOT) publish fails on the non-ASCII repo path; CI builds releases. For a local package test use `-p:DisableAOT=true`.

## Releases
- Public repo: https://github.com/OsmYlmztrk/GitO (`origin`, branch `main` = local `mygitgui`). Don't push upstream's tags.
- Versions are `X.Y.Z`; `VERSION` is the default for local builds, the tag sets the release version. On upstream merges keep our `VERSION`.
- Release: push a tag `vX.Y.Z` (only when asked). `.github/workflows/release.yml` publishes (AOT), packs with Velopack (`vpk`) and creates the GitHub Release with `GitO-win-Setup.exe`, update packages and notes from commit subjects.
- Installed copies update themselves (`Models/AppUpdater.cs`: Velopack + GitHub releases); dev builds don't.

## Working rules
- **Testing:** never drive the user's mouse/keyboard or click in the app or in Unreal. Build, then give the user a short checklist. CLI checks are fine: git scratch repos, Unreal commandlets, reading generated JSON.
- **UI language:** English. Exceptions: Conflict Radar texts and toolbar tooltips are Turkish.
- **Licensing:**
  - Don't copy Fork's branding or icons; reuse layout only.
  - Never copy Unreal Engine source into the repo (EULA); only call engine APIs.
- **Unreal merge window:** buttons only, except graph editing (user request, 2026-10-10): Ctrl+C in any graph; Ctrl+V, Delete, Ctrl+Z, Ctrl+Y in the Result graph. Don't add other shortcuts.
- **Commits:** only when asked. `.mcp.json` (local Unreal MCP config) stays uncommitted.
