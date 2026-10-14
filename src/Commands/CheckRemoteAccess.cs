using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: `git ls-remote --heads` without any prompt (NonInteractive), to find out whether the
    ///     configured Git account may reach an HTTPS remote before fetch / pull / push opens sign-in windows.
    /// </summary>
    public partial class CheckRemoteAccess : Command
    {
        public enum Access
        {
            Ok,
            Denied,  // not signed in, or the account has no permission
            Unknown, // offline, server down...: let the real command report it
        }

        public CheckRemoteAccess(string repo)
        {
            WorkingDirectory = repo;
            Context = repo;
            RaiseError = false;
            NonInteractive = true;
        }

        public async Task<Access> CheckAsync(string remote)
        {
            Args = $"ls-remote --heads {remote.Quoted()}";

            var rs = await ReadToEndAsync().ConfigureAwait(false);
            if (rs.IsSuccess)
                return Access.Ok;

            return REG_DENIED().IsMatch(rs.StdErr) ? Access.Denied : Access.Unknown;
        }

        /// <summary>
        ///     The account Git Credential Manager uses for this URL (credential.&lt;url&gt;.username), or null.
        /// </summary>
        public async Task<string> GetAccountAsync(string url)
        {
            Args = $"config --get-urlmatch credential.username {url.Quoted()}";

            var rs = await ReadToEndAsync().ConfigureAwait(false);
            return rs.IsSuccess ? rs.StdOut.Trim() : null;
        }

        [GeneratedRegex(@"Cannot prompt because|terminal prompts disabled|could not read (Username|Password)|Authentication failed|Repository not found|Permission to .* denied|\b40[13]\b", RegexOptions.IgnoreCase)]
        private static partial Regex REG_DENIED();
    }
}
