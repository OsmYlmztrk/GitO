using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

using Avalonia.Threading;

namespace SourceGit.ViewModels
{
    /// <summary>
    ///     MyGitGUI: finds which of the user's Models.GitAccount entries a repository (or the global config) uses.
    /// </summary>
    public static class GitAccounts
    {
        public record Active(Models.GitAccount Account, bool IsRepositorySpecific, string UserName, string Email);

        public static async Task<Active> FindAsync(string repository)
        {
            var accounts = Preferences.Instance.GitAccounts;
            var global = await new Commands.GitAccountConfig(null).ReadAsync().ConfigureAwait(false);
            var local = string.IsNullOrEmpty(repository)
                ? new System.Collections.Generic.Dictionary<string, string>()
                : await new Commands.GitAccountConfig(repository).ReadAsync().ConfigureAwait(false);

            string Get(string key) => local.TryGetValue(key, out var l) ? l : (global.TryGetValue(key, out var g) ? g : string.Empty);

            var name = Get("user.name");
            var email = Get("user.email");
            var isLocal = local.ContainsKey("user.email") || local.ContainsKey("user.name");

            Models.GitAccount emailMatch = null;
            foreach (var account in accounts)
            {
                if (!string.Equals(account.Email?.Trim(), email, StringComparison.OrdinalIgnoreCase))
                    continue;

                var credential = Get(account.CredentialKey);
                if (string.IsNullOrWhiteSpace(account.CredentialUser) || string.Equals(account.CredentialUser.Trim(), credential, StringComparison.OrdinalIgnoreCase))
                    return new Active(account, isLocal, name, email);

                emailMatch ??= account;
            }

            return new Active(emailMatch, isLocal, name, email);
        }

        /// <summary>
        ///     Adds the GitHub logins saved in Git Credential Manager that are not in the list yet.
        ///     Automatic runs skip logins imported before (the user may have removed them); manual runs don't.
        ///     Name and email come from the global identity when its user.name is the login, otherwise from
        ///     the GitHub API (public name and email, or the account's noreply address). Returns the number added.
        /// </summary>
        public static async Task<int> ImportSavedAsync(bool manual)
        {
            try
            {
                const string host = "github.com";
                var output = await RunGitAsync("credential-manager github list", null).ConfigureAwait(false);
                if (string.IsNullOrWhiteSpace(output))
                    return 0;

                var pref = Preferences.Instance;
                var existing = await Dispatcher.UIThread.InvokeAsync(() => new List<Models.GitAccount>(pref.GitAccounts));
                var global = await new Commands.GitAccountConfig(null).ReadAsync().ConfigureAwait(false);
                global.TryGetValue("user.name", out var globalName);
                global.TryGetValue("user.email", out var globalEmail);

                var added = new List<Models.GitAccount>();
                var imported = new List<string>();
                foreach (var line in output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
                {
                    var login = line.Trim();
                    if (login.Length == 0 || login.Contains(' '))
                        continue;

                    var key = $"{host}/{login}".ToLowerInvariant();
                    imported.Add(key);
                    if (!manual && pref.ImportedGitLogins.Contains(key))
                        continue;
                    if (existing.Exists(x => IsLogin(x, host, login)) || added.Exists(x => IsLogin(x, host, login)))
                        continue;

                    var account = new Models.GitAccount() { Name = login, CredentialUser = login, Host = host };
                    if (string.Equals(globalName, login, StringComparison.OrdinalIgnoreCase) && !string.IsNullOrWhiteSpace(globalEmail))
                    {
                        account.UserName = globalName;
                        account.Email = globalEmail;
                    }
                    else
                    {
                        await FillFromGitHubAsync(account).ConfigureAwait(false);
                    }

                    added.Add(account);
                }

                await Dispatcher.UIThread.InvokeAsync(() =>
                {
                    foreach (var account in added)
                        pref.GitAccounts.Add(account);
                    foreach (var key in imported)
                    {
                        if (!pref.ImportedGitLogins.Contains(key))
                            pref.ImportedGitLogins.Add(key);
                    }
                    pref.Save();
                });

                return added.Count;
            }
            catch (Exception e)
            {
                Native.OS.LogException(e);
                return 0;
            }
        }

        private static bool IsLogin(Models.GitAccount account, string host, string login)
        {
            var accountHost = string.IsNullOrWhiteSpace(account.Host) ? "github.com" : account.Host.Trim();
            return string.Equals(accountHost, host, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(account.CredentialUser?.Trim(), login, StringComparison.OrdinalIgnoreCase);
        }

        private static async Task FillFromGitHubAsync(Models.GitAccount account)
        {
            account.UserName = account.CredentialUser;

            // The saved token only goes to the host it was saved for (api.github.com for github.com).
            var fill = await RunGitAsync("credential fill", $"protocol=https\nhost=github.com\nusername={account.CredentialUser}\n\n").ConfigureAwait(false);
            var token = string.Empty;
            foreach (var line in fill.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
            {
                if (line.StartsWith("password=", StringComparison.Ordinal))
                    token = line.Substring(9);
            }

            if (string.IsNullOrEmpty(token))
                return;

            using var client = new HttpClient() { Timeout = TimeSpan.FromSeconds(15) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("GitO");
            client.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
            client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");

            using var rsp = await client.GetAsync("https://api.github.com/user").ConfigureAwait(false);
            if (!rsp.IsSuccessStatusCode)
                return;

            using var doc = JsonDocument.Parse(await rsp.Content.ReadAsStringAsync().ConfigureAwait(false));
            var root = doc.RootElement;
            if (root.TryGetProperty("name", out var name) && name.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(name.GetString()))
                account.UserName = name.GetString();

            if (root.TryGetProperty("email", out var email) && email.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(email.GetString()))
                account.Email = email.GetString();
            else if (root.TryGetProperty("id", out var id) && root.TryGetProperty("login", out var login))
                account.Email = $"{id.GetInt64()}+{login.GetString()}@users.noreply.github.com";
        }

        private static async Task<string> RunGitAsync(string args, string input)
        {
            var start = new ProcessStartInfo()
            {
                FileName = Native.OS.GitExecutable,
                Arguments = args,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardInput = input != null,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
            };
            start.Environment["GCM_INTERACTIVE"] = "never";
            start.Environment["GIT_TERMINAL_PROMPT"] = "0";

            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            using var proc = Process.Start(start);
            if (proc == null)
                return string.Empty;

            if (input != null)
            {
                await proc.StandardInput.WriteAsync(input).ConfigureAwait(false);
                proc.StandardInput.Close();
            }

            var stdout = proc.StandardOutput.ReadToEndAsync(cts.Token);
            _ = proc.StandardError.ReadToEndAsync(cts.Token);
            try
            {
                await proc.WaitForExitAsync(cts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                proc.Kill(true);
                return string.Empty;
            }

            return proc.ExitCode == 0 ? await stdout.ConfigureAwait(false) : string.Empty;
        }
    }
}
