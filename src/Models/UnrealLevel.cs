using System;
using System.IO;
using System.Threading.Tasks;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: how an Unreal level (.umap) stores its actors. With One File Per Actor ("Use External Actors",
    ///     always on for World Partition levels) every actor is its own file under Content/__ExternalActors__/,
    ///     so people editing different actors don't conflict. GitO only merges levels stored that way.
    /// </summary>
    public static class UnrealLevel
    {
        public const string GuideUrl = "https://github.com/OsmYlmztrk/GitO/blob/main/docs/unreal-guide.md#levels-one-file-per-actor";

        public static bool IsLevelFile(string path)
        {
            return Path.GetExtension(path).Equals(".umap", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        ///     Repo-relative folder of the level's actor files:
        ///     Content/Maps/Forest.umap -> Content/__ExternalActors__/Maps/Forest (plugin content alike).
        ///     Null when the level isn't under a Content folder.
        /// </summary>
        public static string GetExternalActorsFolder(string levelPath)
        {
            var path = levelPath.Replace('\\', '/');
            int content;
            if (path.StartsWith("Content/", StringComparison.OrdinalIgnoreCase))
                content = 0;
            else
            {
                content = path.IndexOf("/Content/", StringComparison.OrdinalIgnoreCase);
                if (content < 0)
                    return null;
                content++;
            }

            var root = path.Substring(0, content + 8);
            var rest = path.Substring(content + 8);
            rest = rest.Substring(0, rest.Length - Path.GetExtension(rest).Length);
            return $"{root}__ExternalActors__/{rest}";
        }

        /// <summary>
        ///     Checks the working copy. A level using One File Per Actor that has no actors at all has no folder yet,
        ///     so it counts as one file until its first actor is saved.
        /// </summary>
        public static bool UsesOneFilePerActor(string repo, string levelPath)
        {
            var folder = GetExternalActorsFolder(levelPath);
            return folder != null && Directory.Exists(Path.Combine(repo, folder));
        }

        /// <summary>
        ///     Same check in a commit, for the two sides of a conflict.
        /// </summary>
        public static async Task<bool> UsesOneFilePerActorAsync(string repo, string revision, string levelPath)
        {
            var folder = GetExternalActorsFolder(levelPath);
            if (folder == null)
                return false;

            return await new Commands.IsPathInRevision(repo, revision, folder).GetResultAsync().ConfigureAwait(false);
        }
    }
}
