using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;

namespace SourceGit.Commands
{
    public class QueryConflictFileState : Command
    {
        public QueryConflictFileState(string repo, Models.Change change)
        {
            var opt = new Models.DiffOption(change, true);

            WorkingDirectory = repo;
            Context = repo;
            Args = $"diff --no-color --no-ext-diff --no-textconv --full-index --patch {opt}";
            _file = Path.Combine(repo, change.Path);
        }

        public async Task<Models.ConflictFileState> GetResultAsync()
        {
            try
            {
                // MyGitGUI: without an LFS merge driver git merges the pointer files as text, leaving a
                // pointer with conflict markers in the working tree. The diff below can't see those markers
                // (the LFS clean filter hides them), so detect the broken pointer here.
                if (IsConflictedLFSPointer(_file))
                    return Models.ConflictFileState.UnmergedBinary;

                using var proc = new Process();
                proc.StartInfo = CreateGitStartInfo(true);
                proc.Start();

                var isBinary = false;
                var tokenCount = 0;

                while (await proc.StandardOutput.ReadLineAsync().ConfigureAwait(false) is { } line)
                {
                    if (isBinary)
                        continue;

                    if (line.StartsWith("Binary files ", StringComparison.Ordinal))
                        isBinary = true;
                    else if (line.StartsWith("++<<<<<<<", StringComparison.Ordinal) ||
                        line.StartsWith("++=======", StringComparison.Ordinal) ||
                        line.StartsWith("++>>>>>>>", StringComparison.Ordinal))
                        tokenCount++;
                }

                await proc.WaitForExitAsync().ConfigureAwait(false);

                if (isBinary)
                    return Models.ConflictFileState.UnmergedBinary;

                return tokenCount == 0 ? Models.ConflictFileState.Resolved : Models.ConflictFileState.UnmergedText;
            }
            catch
            {
                return Models.ConflictFileState.Unknown;
            }
        }

        private static bool IsConflictedLFSPointer(string file)
        {
            var info = new FileInfo(file);
            if (!info.Exists || info.Length > 1024)
                return false;

            var content = File.ReadAllText(file);
            return content.StartsWith("version https://git-lfs.github.com/spec/v1", StringComparison.Ordinal) &&
                content.Contains("<<<<<<<", StringComparison.Ordinal);
        }

        private readonly string _file;
    }
}
