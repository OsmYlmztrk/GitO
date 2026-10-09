using System;
using System.Threading.Tasks;

using Velopack;
using Velopack.Sources;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: self-update through Velopack, reading releases of the GitO GitHub repo. Only works in a copy
    ///     installed with GitO-Setup.exe; a dev build or an unzipped copy reports that updates are unavailable.
    /// </summary>
    public static class AppUpdater
    {
        public const string RepositoryUrl = "https://github.com/OsmYlmztrk/GitO";

        public static bool IsInstalled => Manager.IsInstalled;

        /// <summary>The newer release, or null when this copy is up to date. Throws when GitO wasn't installed with the setup.</summary>
        public static async Task<Version> CheckAsync()
        {
            if (!Manager.IsInstalled)
                throw new InvalidOperationException("Updates only work in a copy installed with GitO-Setup.exe.");

            var info = await Manager.CheckForUpdatesAsync().ConfigureAwait(false);
            if (info == null)
                return null;

            var target = info.TargetFullRelease;
            return new Version()
            {
                Name = $"GitO {target.Version}",
                TagName = $"v{target.Version}",
                Body = target.NotesMarkdown ?? string.Empty,
                Update = info,
            };
        }

        /// <summary>Downloads the release (delta when possible), then closes GitO, installs it and restarts.</summary>
        public static async Task DownloadAndRestartAsync(Version ver, Action<int> progress)
        {
            await Manager.DownloadUpdatesAsync(ver.Update, progress).ConfigureAwait(false);
            Manager.ApplyUpdatesAndRestart(ver.Update.TargetFullRelease);
        }

        private static UpdateManager Manager => _manager ??= new UpdateManager(new GithubSource(RepositoryUrl, null, false));
        private static UpdateManager _manager = null;
    }
}
