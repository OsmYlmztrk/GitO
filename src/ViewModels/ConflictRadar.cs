using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SourceGit.ViewModels
{
    /// <summary>
    ///     MyGitGUI: warns when a file you are changing (uncommitted or unpushed) was also changed by
    ///     someone else on a recently active remote branch. Re-scans after fetch/branch refresh and
    ///     whenever local changes change.
    /// </summary>
    public class ConflictRadar : ObservableObject
    {
        public List<Models.ConflictRisk> Risks
        {
            get => _risks;
            private set
            {
                if (SetProperty(ref _risks, value))
                {
                    OnPropertyChanged(nameof(Count));
                    OnPropertyChanged(nameof(HasRisks));
                    OnPropertyChanged(nameof(HasHighRisk));
                }
            }
        }

        public int Count => _risks.Count;
        public bool HasRisks => _risks.Count > 0;
        public bool HasHighRisk => _risks.Exists(x => x.IsHigh);

        public ObservableCollection<Models.ConflictRisk> Toasts { get; } = [];

        public bool IsScanning
        {
            get => _isScanning;
            private set => SetProperty(ref _isScanning, value);
        }

        public bool IsEnabled
        {
            get => Preferences.Instance.ConflictRadarEnabled;
            set
            {
                if (Preferences.Instance.ConflictRadarEnabled == value)
                    return;

                Preferences.Instance.ConflictRadarEnabled = value;
                OnPropertyChanged();
                if (value)
                    Schedule();
                else
                    ClearAll();
            }
        }

        public bool IsSoundEnabled
        {
            get => Preferences.Instance.ConflictRadarSound;
            set
            {
                if (Preferences.Instance.ConflictRadarSound != value)
                {
                    Preferences.Instance.ConflictRadarSound = value;
                    OnPropertyChanged();
                }
            }
        }

        public bool IsAutoFetchEnabled
        {
            get => Preferences.Instance.EnableAutoFetch;
            set
            {
                if (Preferences.Instance.EnableAutoFetch != value)
                {
                    Preferences.Instance.EnableAutoFetch = value;
                    OnPropertyChanged();
                }
            }
        }

        public int AutoFetchInterval => Preferences.Instance.AutoFetchInterval;

        public int ActiveDays
        {
            get => Preferences.Instance.ConflictRadarDays;
            set
            {
                var days = Math.Clamp(value, 1, 90);
                if (Preferences.Instance.ConflictRadarDays != days)
                {
                    Preferences.Instance.ConflictRadarDays = days;
                    OnPropertyChanged();
                    Schedule();
                }
            }
        }

        /// <summary>Raised on the UI thread with risks that were not reported before.</summary>
        public event Action<List<Models.ConflictRisk>> NewRisksDetected;

        public ConflictRadar(Repository repo)
        {
            _repo = repo;
        }

        public void SetLocalChanges(List<Models.Change> changes)
        {
            var paths = new HashSet<string>(StringComparer.Ordinal);
            foreach (var c in changes)
                paths.Add(c.Path);

            _localPaths = paths;
            Schedule();
        }

        public void SetBranches(List<Models.Branch> branches)
        {
            _branches = branches;
            Schedule();
        }

        public void Ignore(Models.ConflictRisk risk)
        {
            _ignored.Add(IgnoreKey(risk));
            Toasts.Remove(risk);
            Risks = _risks.FindAll(x => !_ignored.Contains(IgnoreKey(x)));
        }

        public void GoToCommit(Models.ConflictRisk risk)
        {
            _repo.NavigateToCommit(risk.SHA);
        }

        public void DismissToast(Models.ConflictRisk risk)
        {
            Toasts.Remove(risk);
        }

        public void Rescan()
        {
            _fileCache.Clear();
            Schedule(0);
        }

        public void Dispose()
        {
            _debounce?.Cancel();
            _debounce = null;
            NewRisksDetected = null;
        }

        private void Schedule(int delayMs = 1500)
        {
            if (!Preferences.Instance.ConflictRadarEnabled)
                return;

            _debounce?.Cancel();
            _debounce = new CancellationTokenSource();
            var token = _debounce.Token;

            Task.Run(async () =>
            {
                try
                {
                    await Task.Delay(delayMs, token).ConfigureAwait(false);
                    await ScanAsync(token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    // Superseded by a newer scan.
                }
            }, token);
        }

        private async Task ScanAsync(CancellationToken token)
        {
            var branches = _branches;
            var local = _localPaths;
            var current = branches?.Find(x => x.IsCurrent);
            if (branches == null || current == null || string.IsNullOrEmpty(current.Head))
                return;

            Dispatcher.UIThread.Post(() => IsScanning = true);

            try
            {
                var repoPath = _repo.FullPath;

                // My side: uncommitted files + files in commits not pushed to the upstream yet.
                var mine = new Dictionary<string, bool>(StringComparer.Ordinal);
                foreach (var p in local)
                    mine[p] = true;

                if (!string.IsNullOrEmpty(current.Upstream) && !current.IsUpstreamGone)
                {
                    var unpushed = await new Commands.QueryChangedFileNames(repoPath, $"{current.Upstream}...HEAD").GetResultAsync().ConfigureAwait(false);
                    foreach (var p in unpushed)
                        mine.TryAdd(p, false);
                }

                token.ThrowIfCancellationRequested();

                if (mine.Count == 0)
                {
                    Publish([], token);
                    return;
                }

                _myEmail ??= await new Commands.Config(repoPath).GetAsync("user.email").ConfigureAwait(false) ?? string.Empty;

                // Their side: remote branches with a commit in the last N days, excluding my own upstream.
                var since = (ulong)DateTimeOffset.UtcNow.AddDays(-Preferences.Instance.ConflictRadarDays).ToUnixTimeSeconds();
                var risks = new List<Models.ConflictRisk>();

                foreach (var b in branches)
                {
                    token.ThrowIfCancellationRequested();

                    if (b.IsLocal || b.Name == "HEAD" || b.CommitterDate < since)
                        continue;
                    if (b.FullName.Equals(current.Upstream, StringComparison.Ordinal) || b.Head == current.Head)
                        continue;

                    var cacheKey = $"{current.Head}:{b.Head}";
                    if (!_fileCache.TryGetValue(cacheKey, out var theirs))
                    {
                        theirs = await new Commands.QueryChangedFileNames(repoPath, $"HEAD...{b.FullName}").GetResultAsync().ConfigureAwait(false);
                        _fileCache[cacheKey] = theirs;
                    }

                    foreach (var path in theirs)
                    {
                        if (!mine.TryGetValue(path, out var isUncommitted))
                            continue;

                        var touch = await new Commands.QueryLastFileTouch(repoPath, $"HEAD..{b.FullName}", path).GetResultAsync().ConfigureAwait(false);
                        if (touch is not { } t || string.IsNullOrEmpty(t.SHA))
                            continue;

                        // Your own work on another branch is not a conflict with a teammate.
                        if (!string.IsNullOrEmpty(_myEmail) && t.Email.Equals(_myEmail, StringComparison.OrdinalIgnoreCase))
                            continue;

                        var level = Models.ConflictRisk.LooksBinary(path) ||
                                    await new Commands.IsLFSFiltered(repoPath, path).GetResultAsync().ConfigureAwait(false)
                            ? Models.ConflictRiskLevel.High
                            : Models.ConflictRiskLevel.Medium;

                        risks.Add(new Models.ConflictRisk()
                        {
                            Path = path,
                            Branch = b.FriendlyName,
                            SHA = t.SHA,
                            Author = t.Author,
                            AuthorEmail = t.Email,
                            Time = t.Time,
                            Level = level,
                            IsUncommitted = isUncommitted,
                        });
                    }
                }

                risks.Sort((l, r) => l.Level != r.Level ? r.Level.CompareTo(l.Level) : r.Time.CompareTo(l.Time));
                Publish(risks, token);
            }
            finally
            {
                Dispatcher.UIThread.Post(() => IsScanning = false);
            }
        }

        private void Publish(List<Models.ConflictRisk> risks, CancellationToken token)
        {
            Dispatcher.UIThread.Post(() =>
            {
                if (token.IsCancellationRequested)
                    return;

                var visible = risks.FindAll(x => !_ignored.Contains(IgnoreKey(x)));
                var fresh = visible.FindAll(x => _reported.Add(x.Key));

                // Drop toasts for risks that are gone (e.g. file committed & pushed, branch merged).
                for (var i = Toasts.Count - 1; i >= 0; i--)
                {
                    if (!visible.Exists(x => x.Key == Toasts[i].Key))
                        Toasts.RemoveAt(i);
                }

                Risks = visible;

                if (fresh.Count > 0)
                {
                    foreach (var r in fresh)
                    {
                        Toasts.Insert(0, r);
                        while (Toasts.Count > MaxToasts)
                            Toasts.RemoveAt(Toasts.Count - 1);
                    }

                    NewRisksDetected?.Invoke(fresh);
                }
            });
        }

        private void ClearAll()
        {
            _debounce?.Cancel();
            Toasts.Clear();
            Risks = [];
        }

        // Ignoring is per file + branch, so later commits on the same branch stay quiet too.
        private static string IgnoreKey(Models.ConflictRisk r) => $"{r.Branch}|{r.Path}";

        private const int MaxToasts = 3;

        private readonly Repository _repo;
        private List<Models.Branch> _branches = null;
        private HashSet<string> _localPaths = [];
        private List<Models.ConflictRisk> _risks = [];
        private bool _isScanning = false;
        private string _myEmail = null;
        private CancellationTokenSource _debounce = null;
        private readonly ConcurrentDictionary<string, HashSet<string>> _fileCache = new(StringComparer.Ordinal);
        private readonly HashSet<string> _reported = new(StringComparer.Ordinal);
        private readonly HashSet<string> _ignored = new(StringComparer.Ordinal);
    }
}
