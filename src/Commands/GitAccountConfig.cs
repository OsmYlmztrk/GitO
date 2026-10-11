using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: reads and writes the keys of a Models.GitAccount in one config scope
    ///     (--local for a repository, --global when repository is null).
    /// </summary>
    public class GitAccountConfig : Command
    {
        public GitAccountConfig(string repository)
        {
            if (string.IsNullOrEmpty(repository))
            {
                WorkingDirectory = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
                _scope = "--global";
            }
            else
            {
                WorkingDirectory = repository;
                Context = repository;
                _scope = "--local";
            }
        }

        public async Task<Dictionary<string, string>> ReadAsync()
        {
            Args = $"config {_scope} -l";
            RaiseError = false;

            var rs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var output = await ReadToEndAsync().ConfigureAwait(false);
            if (!output.IsSuccess)
                return rs;

            var lines = output.StdOut.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
            foreach (var line in lines)
            {
                var parts = line.Split('=', 2);
                if (parts.Length == 2)
                    rs[parts[0]] = parts[1];
            }

            return rs;
        }

        public async Task ApplyAsync(Models.GitAccount account)
        {
            var current = await ReadAsync().ConfigureAwait(false);
            await SetOrUnsetAsync(current, "user.name", account.UserName).ConfigureAwait(false);
            await SetOrUnsetAsync(current, "user.email", account.Email).ConfigureAwait(false);
            await SetOrUnsetAsync(current, account.CredentialKey, account.CredentialUser).ConfigureAwait(false);
        }

        public async Task ClearAsync(IEnumerable<Models.GitAccount> accounts)
        {
            var current = await ReadAsync().ConfigureAwait(false);
            await SetOrUnsetAsync(current, "user.name", null).ConfigureAwait(false);
            await SetOrUnsetAsync(current, "user.email", null).ConfigureAwait(false);

            var keys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var account in accounts)
                keys.Add(account.CredentialKey);
            foreach (var key in keys)
                await SetOrUnsetAsync(current, key, null).ConfigureAwait(false);
        }

        private async Task SetOrUnsetAsync(Dictionary<string, string> current, string key, string value)
        {
            current.TryGetValue(key, out var old);
            if (string.IsNullOrWhiteSpace(value))
            {
                if (old == null)
                    return;

                Args = $"config {_scope} --unset-all {key}";
            }
            else
            {
                value = value.Trim();
                if (old == value)
                    return;

                Args = $"config {_scope} {key} {value.Quoted()}";
            }

            RaiseError = true;
            await ExecAsync().ConfigureAwait(false);
        }

        private readonly string _scope;
    }
}
