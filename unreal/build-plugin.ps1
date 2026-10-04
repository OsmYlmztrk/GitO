# Builds the GitBlueprintMerge Unreal plugin for one engine version and installs it where MyGitGUI
# looks for it: %APPDATA%\MyGitGUI\UnrealPlugins\<EngineVersion>\GitBlueprintMerge
#
#   powershell -ExecutionPolicy Bypass -File unreal\build-plugin.ps1 -EngineVersion 5.8
#
# Needs the engine (Epic Games Launcher install) and Visual Studio with the C++ workload.

param(
    [string]$EngineVersion = "5.8",
    [string]$EngineDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $EngineDir) {
    $manifest = Join-Path $env:ProgramData "Epic\UnrealEngineLauncher\LauncherInstalled.dat"
    if (Test-Path $manifest) {
        $install = (Get-Content $manifest -Raw | ConvertFrom-Json).InstallationList |
            Where-Object { $_.AppName -eq "UE_$EngineVersion" } | Select-Object -First 1
        if ($install) { $EngineDir = $install.InstallLocation }
    }
    if (-not $EngineDir) { $EngineDir = Join-Path $env:ProgramFiles "Epic Games\UE_$EngineVersion" }
}

$runUAT = Join-Path $EngineDir "Engine\Build\BatchFiles\RunUAT.bat"
if (-not (Test-Path $runUAT)) { throw "Unreal Engine $EngineVersion not found (looked in '$EngineDir'). Pass -EngineDir." }

# Build from a short ASCII path: UBT dislikes long or non-ASCII paths (e.g. OneDrive\Masaüstü).
$work = Join-Path $env:LOCALAPPDATA "Temp\gbm-$EngineVersion"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force "$work\src" | Out-Null
Copy-Item -Recurse (Join-Path $PSScriptRoot "GitBlueprintMerge") "$work\src\"

& $runUAT BuildPlugin -Plugin="$work\src\GitBlueprintMerge\GitBlueprintMerge.uplugin" -Package="$work\pkg" -TargetPlatforms=Win64
if ($LASTEXITCODE -ne 0) { throw "Plugin build failed (exit $LASTEXITCODE)." }

$target = Join-Path $env:APPDATA "MyGitGUI\UnrealPlugins\$EngineVersion\GitBlueprintMerge"
if (Test-Path $target) { Remove-Item -Recurse -Force $target }
New-Item -ItemType Directory -Force $target | Out-Null
Get-ChildItem "$work\pkg" | Where-Object { $_.Name -notin @("HostProject", "Intermediate") } |
    ForEach-Object { Copy-Item -Recurse $_.FullName $target }

Write-Host "Installed GitBlueprintMerge for UE $EngineVersion to $target"
