using System;
using System.Collections.Generic;
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
}
