using System;
using System.IO;
using System.Runtime.Versioning;
using System.Text.Json;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: locates the Unreal project and editor for an asset, so conflicted
    ///     .uasset/.umap files can be opened in Unreal's own diff/merge tool
    ///     (UnrealEditor.exe Project.uproject -diff ...).
    /// </summary>
    public static class UnrealEngine
    {
        public static bool IsAssetFile(string path)
        {
            var ext = Path.GetExtension(path);
            return ext.Equals(".uasset", StringComparison.OrdinalIgnoreCase) ||
                ext.Equals(".umap", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        ///     Walks up from the asset's folder and returns the first .uproject found.
        /// </summary>
        public static string FindProject(string assetFullPath)
        {
            var dir = Path.GetDirectoryName(assetFullPath);
            while (!string.IsNullOrEmpty(dir))
            {
                try
                {
                    var projects = Directory.GetFiles(dir, "*.uproject", SearchOption.TopDirectoryOnly);
                    if (projects.Length > 0)
                        return projects[0];
                }
                catch
                {
                    // Ignore unreadable folders and keep walking up.
                }

                dir = Path.GetDirectoryName(dir);
            }

            return null;
        }

        /// <summary>
        ///     Resolves UnrealEditor.exe from the project's EngineAssociation (launcher version like "5.8"
        ///     or a source-build GUID). Returns null when it can't be found.
        /// </summary>
        public static string FindEditor(string uprojectPath)
        {
            if (!OperatingSystem.IsWindows())
                return null;

            var association = GetEngineAssociation(uprojectPath);
            if (string.IsNullOrEmpty(association))
                return null;

            foreach (var root in FindEngineRoots(association))
            {
                if (string.IsNullOrEmpty(root))
                    continue;

                var exe = Path.Combine(root, "Engine", "Binaries", "Win64", "UnrealEditor.exe");
                if (File.Exists(exe))
                    return exe;
            }

            return null;
        }

        /// <summary>
        ///     The GitBlueprintMerge plugin built for this project's engine, installed under
        ///     %APPDATA%/MyGitGUI/UnrealPlugins/&lt;EngineAssociation&gt;/ by unreal/build-plugin.ps1.
        /// </summary>
        public static string FindMergePlugin(string uprojectPath)
        {
            var association = GetEngineAssociation(uprojectPath);
            if (string.IsNullOrEmpty(association))
                return null;

            var plugin = Path.Combine(Native.OS.BasicDirectories.ConfigDir, "UnrealPlugins", association.Trim('{', '}'), "GitBlueprintMerge", "GitBlueprintMerge.uplugin");
            return File.Exists(plugin) ? plugin : null;
        }

        /// <summary>
        ///     Long package name of an asset: Content/A/B.uasset -> /Game/A/B, Plugins/X/Content/A/B.uasset -> /X/A/B.
        /// </summary>
        public static string GetPackageName(string uprojectPath, string assetFullPath)
        {
            var projectDir = Path.GetDirectoryName(uprojectPath);
            var relative = Path.GetRelativePath(projectDir, assetFullPath).Replace('\\', '/');
            var withoutExt = relative.Substring(0, relative.Length - Path.GetExtension(relative).Length);

            if (withoutExt.StartsWith("Content/", StringComparison.OrdinalIgnoreCase))
                return "/Game/" + withoutExt.Substring(8);

            var content = withoutExt.LastIndexOf("/Content/", StringComparison.OrdinalIgnoreCase);
            if (content <= 0)
                return null;

            var mount = withoutExt.Substring(0, content);
            mount = mount.Substring(mount.LastIndexOf('/') + 1);
            return $"/{mount}/{withoutExt.Substring(content + 9)}";
        }

        private static string GetEngineAssociation(string uprojectPath)
        {
            if (string.IsNullOrEmpty(uprojectPath))
                return null;

            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(uprojectPath));
                return doc.RootElement.TryGetProperty("EngineAssociation", out var prop) ? prop.GetString() : null;
            }
            catch
            {
                return null;
            }
        }

        [SupportedOSPlatform("windows")]
        private static string[] FindEngineRoots(string association)
        {
            // Source builds register themselves under HKCU with a GUID.
            if (association.StartsWith('{'))
            {
                using var builds = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Epic Games\Unreal Engine\Builds");
                return [builds?.GetValue(association) as string];
            }

            return [
                FindLauncherInstall(association),
                FindRegisteredInstall(association),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Epic Games", $"UE_{association}"),
            ];
        }

        private static string FindLauncherInstall(string version)
        {
            try
            {
                var manifest = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "Epic", "UnrealEngineLauncher", "LauncherInstalled.dat");
                if (!File.Exists(manifest))
                    return null;

                using var doc = JsonDocument.Parse(File.ReadAllText(manifest));
                if (!doc.RootElement.TryGetProperty("InstallationList", out var list))
                    return null;

                foreach (var item in list.EnumerateArray())
                {
                    if (item.TryGetProperty("AppName", out var name) && name.GetString() == $"UE_{version}" &&
                        item.TryGetProperty("InstallLocation", out var location))
                        return location.GetString();
                }
            }
            catch
            {
                // Fall through to the other lookups.
            }

            return null;
        }

        [SupportedOSPlatform("windows")]
        private static string FindRegisteredInstall(string version)
        {
            using var key = Microsoft.Win32.RegistryKey
                .OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry64)
                .OpenSubKey($@"SOFTWARE\EpicGames\Unreal Engine\{version}");
            return key?.GetValue("InstalledDirectory") as string;
        }
    }
}
