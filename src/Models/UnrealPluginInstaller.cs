using System;
using System.IO;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: copies the prebuilt GitBlueprintMerge plugin shipped next to GitO.exe (UnrealPlugins/&lt;engine&gt;/)
    ///     to &lt;config dir&gt;/UnrealPlugins/&lt;engine&gt;/, where <see cref="UnrealEngine.FindMergePlugin"/> looks.
    ///     The plugin is used from there, not from the install folder, so an update of GitO never has to replace a DLL
    ///     that a running Unreal editor holds. A copy that is newer than the bundled one (a local build by
    ///     unreal/build-plugin.ps1) is left alone.
    /// </summary>
    public static class UnrealPluginInstaller
    {
        public static void Install()
        {
            try
            {
                var bundled = Path.Combine(AppContext.BaseDirectory, "UnrealPlugins");
                if (!Directory.Exists(bundled))
                    return;

                foreach (var engineDir in Directory.GetDirectories(bundled))
                {
                    var source = Path.Combine(engineDir, PluginName);
                    if (File.Exists(Path.Combine(source, PluginName + ".uplugin")))
                        InstallOne(source, Path.Combine(Native.OS.BasicDirectories.ConfigDir, "UnrealPlugins", Path.GetFileName(engineDir), PluginName));
                }
            }
            catch (Exception e)
            {
                Native.OS.LogException(e);
            }
        }

        private static void InstallOne(string source, string target)
        {
            var sourceDll = Path.Combine(source, MainDll);
            var targetDll = Path.Combine(target, MainDll);
            if (File.Exists(targetDll) && File.GetLastWriteTimeUtc(targetDll) >= File.GetLastWriteTimeUtc(sourceDll))
                return;

            try
            {
                foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
                {
                    var dest = Path.Combine(target, Path.GetRelativePath(source, file));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);

                    // Copy beside, then swap in, so a stopped copy never leaves a broken DLL behind.
                    var temp = dest + ".tmp";
                    File.Copy(file, temp, true);
                    File.Move(temp, dest, true);
                    File.SetLastWriteTimeUtc(dest, File.GetLastWriteTimeUtc(file));
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // Unreal is running with the plugin loaded; try again on the next start.
            }
        }

        private const string PluginName = "GitBlueprintMerge";
        private static readonly string MainDll = Path.Combine("Binaries", "Win64", "UnrealEditor-GitBlueprintMerge.dll");
    }
}
