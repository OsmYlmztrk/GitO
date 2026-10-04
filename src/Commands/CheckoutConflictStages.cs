using System.IO;
using System.Threading.Tasks;

namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: writes Base / Ours / Theirs of a conflicted file to temp files
    ///     (`git checkout-index --stage=all --temp`). Filters run, so LFS files come out with real content.
    ///     Missing stages (e.g. no base for both-added) are returned as null.
    /// </summary>
    public class CheckoutConflictStages : Command
    {
        public CheckoutConflictStages(string repo, string file)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"checkout-index --stage=all --temp -- {file.Quoted()}";
        }

        public async Task<(string Base, string Ours, string Theirs)> GetResultAsync()
        {
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (!rs.IsSuccess)
            {
                RaiseException(rs.StdErr);
                return (null, null, null);
            }

            // Format: "<base> <ours> <theirs>\t<path>", with "." for a missing stage.
            var line = rs.StdOut.Trim();
            var tab = line.IndexOf('\t');
            var temps = (tab > 0 ? line.Substring(0, tab) : line).Split(' ');
            if (temps.Length != 3)
                return (null, null, null);

            return (ToFullPath(temps[0]), ToFullPath(temps[1]), ToFullPath(temps[2]));
        }

        private string ToFullPath(string temp)
        {
            return temp == "." ? null : Path.Combine(WorkingDirectory, temp);
        }
    }
}
