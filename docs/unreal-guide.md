# GitO for Unreal Engine: setup and use

This guide is for people who use GitO on Unreal Engine projects. It covers what to install, how to set up the project repository, and how to resolve conflicted assets.

**Short version:** install Unreal Engine 5.8 from the Epic Games Launcher, install Git and Git LFS, install GitO. There is nothing to install inside Unreal and nothing to add to your project: GitO brings its merge plugin and starts Unreal with it only while it merges.

## 1. What GitO does for Unreal

| Feature | What it does | Needs |
|---|---|---|
| **Merge in Unreal** | Three-way merge of a conflicted `.uasset`, change by change: Unreal combines both branches in the background, and a merge window opens only when the same thing was changed on both sides. | UE 5.8 (Launcher) |
| **Compare in Unreal** | Opens Unreal's diff with Ours on the left and Theirs on the right. | Any UE version GitO can find |
| **Unreal's merge window** | Fallback for assets GitO's plugin can't read (e.g. levels): Unreal's own window, which keeps one side. | Any UE version GitO can find |
| **LFS locks** | Lock, unlock and see who locked a file, from the conflict panel. | Git LFS, a server with LFS locking |
| **Conflict Radar** | Warns when a file you are changing is also being changed on another branch, or is locked by someone else. | Git LFS for the lock part |

### Which assets merge change by change

| Asset | Merged change by change | Listed for you to decide (can't be taken from theirs yet) |
|---|---|---|
| **Blueprint** (also Widget and Anim Blueprints) | nodes, wires, pin values, node settings, member variables, function graphs (add, remove, rename), Class Defaults, Class Settings, properties of components that exist on both sides, widget and slot settings, widgets added to an existing panel | parent class, interfaces, event graphs and state machines added or removed, components added/removed/moved, widgets removed or moved, widget animations |
| **Data Asset** | every editable property | asset class change, instanced objects |
| **Data Table** | rows added or removed, every row field | row struct change |
| **Material Instance** | parameter values, static switches, parent, base property overrides | material layers, static component masks |

Anything in the right column is shown in the merge window as "can't merge automatically". You can leave it out, which keeps your (ours) version of that part.

Not merged change by change yet: timelines, inherited component overrides, function local variables, and levels (`.umap`). Levels go to Unreal's own merge window.

## 2. Install

1. **Git for Windows** 2.25.1 or later: https://git-scm.com/download/win. Git LFS comes with it. Run `git lfs install` once in a terminal.
2. **Unreal Engine 5.8** from the Epic Games Launcher, in its default place or any folder: GitO finds it through the Launcher's install list. You need your own Epic license; GitO doesn't include any part of the engine.
3. **GitO**: run `GitO-win-Setup.exe` from the [latest release](https://github.com/OsmYlmztrk/GitO/releases/latest). It installs for your user (no admin rights) and updates itself.

That's all. Nothing goes into the engine folder or into your project.

### Where the merge plugin lives

GitO ships the GitBlueprintMerge plugin (built for UE 5.8) and copies it on startup to:

```
%APPDATA%\MyGitGUI\UnrealPlugins\5.8\GitBlueprintMerge\
```

- Unreal loads it from there only while GitO merges (`-PLUGIN=... -EnablePlugins=GitBlueprintMerge`). When you open your project normally, the plugin isn't loaded.
- Your `.uproject` and `Plugins` folder are never changed, so teammates who don't use GitO see no difference.
- To reinstall it, close Unreal, delete that folder and start GitO again.

### Engine versions

| Your project uses | Merge in Unreal | Compare / Unreal's merge window |
|---|---|---|
| UE 5.8 from the Launcher | ✅ | ✅ |
| Another Launcher version (5.7, 5.9, ...) | ❌ falls back to Unreal's window | ✅ |
| A source-built engine (`EngineAssociation` is a `{GUID}`) | ❌ falls back to Unreal's window | ✅ if the build is registered (it registers itself when you run `Setup.bat` / `GenerateProjectFiles.bat`) |

GitO reads the engine version from the `EngineAssociation` field of the `.uproject`. It finds the project by walking up from the conflicted asset's folder, so the `.uproject` can sit anywhere above `Content`.

## 3. Set up the project repository

GitO works with any Git repository. For Unreal projects these settings avoid most problems; set them once, at the start, and commit them.

### `.gitattributes`: store assets in Git LFS and make them lockable

```gitattributes
# Unreal binary assets: LFS, and lockable so two people don't edit the same asset
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
*.umap   filter=lfs diff=lfs merge=lfs -text lockable

# Other large binaries
*.fbx  filter=lfs diff=lfs merge=lfs -text
*.png  filter=lfs diff=lfs merge=lfs -text
*.wav  filter=lfs diff=lfs merge=lfs -text
*.psd  filter=lfs diff=lfs merge=lfs -text
```

- `lockable` turns on LFS locking for these files. GitO's lock buttons and the lock part of Conflict Radar only work for lockable files.
- Lockable files are read-only on disk until you lock them. Unreal then asks to "make writable" when you save; lock the asset in GitO first (conflict panel → **Lock**) so others see you are working on it.
- Your Git server must support LFS locking (GitHub, GitLab, Gitea and Azure DevOps do).

### `.gitignore`: keep generated folders out

```gitignore
Binaries/
Intermediate/
Saved/
DerivedDataCache/
.vs/
*.sln
Plugins/*/Binaries/
Plugins/*/Intermediate/
```

### Unreal's own source control setting

GitO doesn't need Unreal's built-in Source Control (Git) integration. You can leave it off. If you turn it on, use the same repository; it doesn't affect GitO's merge.

## 4. Resolve a conflicted asset

A conflict happens when you merge, rebase, cherry-pick or pull and both branches changed the same `.uasset`.

**Before you start: save your work in Unreal and close the editor** (at least close the asset). GitO writes the merged file over the one in your project; an editor that still has the old asset open can overwrite the result when it saves.

1. In GitO, open the conflicted file in **Local Changes**. The conflict panel shows an **Unreal Editor** card with the asset's project and engine.
2. Optional: **Compare in Unreal** opens Unreal's diff with Ours (your branch) on the left and Theirs (the incoming branch) on the right.
3. Press **Merge in Unreal**. A step list shows what is happening:
   1. GitO extracts the Base, Ours and Theirs versions of the asset (LFS files are downloaded if needed).
   2. Unreal merges them in the background, without a window. This takes as long as starting the editor once.
   3. **No conflicts:** the merged asset is saved into your project and staged. Done.
   4. **Conflicts, or changes that can't be taken automatically:** Unreal opens with the merge window (see below). When you press **Merge** there, the result is saved and staged, and Unreal closes.
4. Check the asset in Unreal, then commit (or continue the rebase) in GitO.

If the plugin can't read the asset (for example a level), GitO opens Unreal's own merge window instead, which keeps one whole side.

### The merge window

- **Left panel:** one card per conflict, with **Keep ours**, **Take theirs** and, where it makes sense (nodes inserted into the same wire), **Take both**. Below the cards, the list of changes that were merged automatically.
- **Columns:** Ours, Result and Theirs, plus Base when you turn it on. Four layouts: Top, Side, Classic and Result.
- **Result is editable:** move nodes, draw wires, change pin values and details; copy nodes from Ours, Base or Theirs into Result with **Copy selected to result** or by dragging them in.
- **Keyboard:** Ctrl+C in any graph; Ctrl+V, Delete, Ctrl+Z and Ctrl+Y in the Result graph. Everything else is done with buttons.
- **Edits must be confirmed:** after editing Result by hand, press **Confirm my edits** (it compiles the Blueprint). **Undo last edit** and **Revert all edits** are next to it. Choosing a different option on a card rebuilds Result, so GitO asks before it drops your hand edits.
- **Merge** saves the result and closes Unreal. Closing the window without merging leaves the file conflicted, so you can try again.

## 5. Troubleshooting

| Problem | What to do |
|---|---|
| No **Unreal Editor** card on a `.uasset` conflict | GitO didn't find a `.uproject` above the asset's folder. Check the asset is inside the project folder. |
| "Unreal Editor for this project's engine version was not found." | Check the `EngineAssociation` in the `.uproject` matches an installed engine (`5.8` for a Launcher install). For a source build, run `Setup.bat` once so it registers. |
| **Merge in Unreal** opens Unreal's "keep one side" window | The project isn't on UE 5.8 from the Launcher, or the asset type isn't supported. The card's text tells you which. |
| Plugin folder missing | Start GitO once (it copies the plugin on startup). If Unreal was running, close it and start GitO again. |
| "cannot be saved as it has only been partially loaded" | The asset is open in another Unreal editor. Close the editor and run the merge again. |
| Unreal closed before the merge finished | Read the log at `%TEMP%\MyGitGUI\Unreal\last-merge.log`. Project errors unrelated to the asset can appear there too; the merge result is what counts. |
| The first merge is slow | Unreal compiles shaders and loads the project the first time, like a normal editor start. |
| Windows SmartScreen warns about the setup | The installer isn't code-signed yet: "More info" → "Run anyway". |
