using System.Threading.Tasks;

namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: whether a file or folder exists in a commit (git ls-tree).
    /// </summary>
    public class IsPathInRevision : Command
    {
        public IsPathInRevision(string repo, string revision, string path)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"ls-tree --name-only {revision} -- {path.Quoted()}";
            RaiseError = false;
        }

        public async Task<bool> GetResultAsync()
        {
            var rs = await ReadToEndAsync().ConfigureAwait(false);
            return rs.IsSuccess && !string.IsNullOrWhiteSpace(rs.StdOut);
        }
    }
}
