using System;
using System.Collections.Generic;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: `git diff --name-only` for a revision range, e.g. `HEAD...origin/feature/x`
    ///     (files changed on the right side since the merge base).
    /// </summary>
    public class QueryChangedFileNames : Command
    {
        public QueryChangedFileNames(string repo, string range)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"diff --name-only --no-renames -z {range}";
            RaiseError = false;
        }

        public async Task<HashSet<string>> GetResultAsync()
        {
            var set = new HashSet<string>(StringComparer.Ordinal);
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (!rs.IsSuccess)
                return set;

            foreach (var path in rs.StdOut.Split('\0', StringSplitOptions.RemoveEmptyEntries))
                set.Add(path.Trim());

            return set;
        }
    }

    /// <summary>
    ///     MyGitGUI: the newest commit in a range that touched a file (SHA, author, email, time).
    /// </summary>
    public class QueryLastFileTouch : Command
    {
        public QueryLastFileTouch(string repo, string range, string path)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"log -1 --no-show-signature --format=%H%x00%an%x00%ae%x00%ct {range} -- {path.Quoted()}";
            RaiseError = false;
        }

        public async Task<(string SHA, string Author, string Email, ulong Time)?> GetResultAsync()
        {
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (!rs.IsSuccess)
                return null;

            var parts = rs.StdOut.Trim().Split('\0');
            if (parts.Length < 4)
                return null;

            ulong.TryParse(parts[3], out var time);
            return (parts[0], parts[1], parts[2], time);
        }
    }

    /// <summary>
    ///     MyGitGUI: old-side line ranges of a `git diff -U0` (relative to the first revision),
    ///     used to tell "same lines changed" from "different parts of the file changed".
    /// </summary>
    public partial class QueryDiffOldRanges : Command
    {
        [GeneratedRegex(@"^@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@")]
        private static partial Regex REG_HUNK();

        /// <param name="revisions">e.g. `base branch` (two commits) or just `base` (against the working tree)</param>
        public QueryDiffOldRanges(string repo, string revisions, string path)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"diff -U0 --no-color --no-ext-diff {revisions} -- {path.Quoted()}";
            RaiseError = false;
        }

        public async Task<List<(int Start, int End)>> GetResultAsync()
        {
            var ranges = new List<(int, int)>();
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (!rs.IsSuccess)
                return ranges;

            foreach (var line in rs.StdOut.Split('\n'))
            {
                var m = REG_HUNK().Match(line);
                if (!m.Success)
                    continue;

                var start = int.Parse(m.Groups[1].Value);
                var count = m.Groups[2].Success ? int.Parse(m.Groups[2].Value) : 1;

                // A pure insertion (count 0) sits right after `start`.
                ranges.Add(count == 0 ? (start, start + 1) : (start, start + count - 1));
            }

            return ranges;
        }

        public static bool Overlap(List<(int Start, int End)> a, List<(int Start, int End)> b, int margin = 2)
        {
            foreach (var x in a)
            {
                foreach (var y in b)
                {
                    if (x.Start <= y.End + margin && y.Start <= x.End + margin)
                        return true;
                }
            }

            return false;
        }
    }

    /// <summary>
    ///     MyGitGUI: which of the given paths have the `lockable` git attribute (Git LFS file locking).
    /// </summary>
    public class QueryLockableFiles : Command
    {
        public QueryLockableFiles(string repo, List<string> paths)
        {
            WorkingDirectory = repo;
            Context = repo;
            RaiseError = false;

            var builder = new StringBuilder("check-attr -z lockable --");
            foreach (var p in paths)
                builder.Append(' ').Append(p.Quoted());
            Args = builder.ToString();
        }

        public async Task<HashSet<string>> GetResultAsync()
        {
            var set = new HashSet<string>(StringComparer.Ordinal);
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (!rs.IsSuccess)
                return set;

            // Output: <path> NUL lockable NUL <value> NUL ...
            var parts = rs.StdOut.Split('\0');
            for (var i = 0; i + 2 < parts.Length; i += 3)
            {
                if (parts[i + 2] is "set" or "true")
                    set.Add(parts[i]);
            }

            return set;
        }
    }
}
