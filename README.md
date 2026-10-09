# GitO

A Windows Git GUI for Unreal and Unity game projects that use Git LFS.

Source and releases: https://github.com/OsmYlmztrk/GitO

GitO is based on [SourceGit](https://github.com/sourcegit-scm/sourcegit), an open-source Git GUI client (MIT License). Thanks to the SourceGit authors and contributors: GitO keeps their code base and adds game-project features on top. GitO is not affiliated with or endorsed by the SourceGit project.

## Features added by GitO

* **Unreal asset merge:** three-way merge of conflicted `.uasset` files change by change (Blueprint incl. Widget and Anim Blueprints, Data Asset, Data Table, Material Instance), through the included Unreal Engine 5.8 editor plugin. Changes that can't be merged automatically are shown, not dropped silently.
* **Conflict Radar:** warns when a file you are changing is also being changed by someone else, on a recently active remote branch or behind an LFS lock.
* A compact commit, branch and conflict layout for daily work on large repositories.

Everything else (history, staging, diff, stash, worktrees, LFS, themes, and more) comes from SourceGit.

## Requirements

* Windows 10/11 (64-bit)
* [Git](https://git-scm.com/download/win) 2.25.1 or later
* For Unreal asset merge: Unreal Engine 5.8 installed (your own Epic license); the plugin is built with `unreal/build-plugin.ps1`

## Build

```
dotnet build src/SourceGit.csproj -c Debug
```

Unreal plugin:

```
powershell -ExecutionPolicy Bypass -File unreal\build-plugin.ps1 -EngineVersion 5.8
```

## Built with AI

GitO's changes to SourceGit and the Unreal plugin were written with AI assistance ([Claude Code](https://claude.com/claude-code) by Anthropic); commits carry a `Co-Authored-By: Claude` line. The app icon was generated with ChatGPT (OpenAI). Osman Yılmaztürk directs, reviews and tests the work.

## License

[MIT](LICENSE). SourceGit's copyright notice is kept in `LICENSE` together with GitO's. Third-party libraries, fonts, grammar files and icons keep their own licenses, listed in [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md). Both files ship with the app.

Unreal Engine is a trademark of Epic Games, Inc. The plugin only calls engine APIs and contains no engine source; you need your own Unreal Engine license to use it.
