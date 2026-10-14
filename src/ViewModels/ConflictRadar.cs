using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SourceGit.ViewModels
{
    /// <summary>
    ///     MyGitGUI: warns when a file you are changing (uncommitted or unpushed) is also being changed by
    ///     someone else — on a recently active remote branch, or behind an LFS lock. Keeps a persistent
    ///     notification history, ignore rules and a snooze timer per repository.
    /// </summary>
    public class ConflictRadar : ObservableObject
    {
        /// <summary>Raised on the UI thread for new alerts of any open repository (sound / taskbar flash).</summary>
        public static event Action<ConflictRadar, List<Models.ConflictRisk>, bool> AlertRaised;

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
                    OnPropertyChanged(nameof(HasAlert));
                }
            }
        }

        public int Count => _risks.Count;
        public bool HasRisks => _risks.Count > 0;
        public bool HasHighRisk => _risks.Exists(x => x.IsHigh);
        public bool HasAlert => _risks.Exists(x => x.IsAlert);

        public ObservableCollection<Models.ConflictRisk> Toasts { get; } = [];

        public List<Models.ConflictNotificationRecord> History
        {
            get
            {
                var list = new List<Models.ConflictNotificationRecord>(_store.History);
                list.Reverse();
                return list;
            }
        }

        public bool HasHistory => _store.History.Count > 0;

        public List<Models.ConflictIgnoreRule> IgnoreRules => new(_store.IgnoreRules);
        public bool HasIgnoreRules => _store.IgnoreRules.Count > 0;

        public bool IsSnoozed => _store.SnoozeUntil > DateTime.Now;
        public string SnoozeText => IsSnoozed ? $"{_store.SnoozeUntil:HH:mm}'e kadar sessiz" : string.Empty;

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

        public ConflictRadar(Repository repo)
        {
            _repo = repo;
            _store = Models.ConflictRadarStore.Load(repo.GitDir);
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

        public void Rescan()
        {
            _fileCache.Clear();
            Schedule(0);
        }

        public void GoToCommit(Models.ConflictRisk risk)
        {
            if (risk.CanGoToCommit)
                _repo.NavigateToCommit(risk.SHA);
        }

        public void GoToCommit(Models.ConflictNotificationRecord record)
        {
            if (record.CanGoToCommit)
                _repo.NavigateToCommit(record.SHA);
        }

        public void DismissToast(Models.ConflictRisk risk)
        {
            Toasts.Remove(risk);
            SetStatus(risk.Key, Models.ConflictNotificationStatus.Dismissed);
            SaveStore();
        }

        public void Ignore(Models.ConflictRisk risk, Models.ConflictIgnoreRuleType type)
        {
            var value = type switch
            {
                Models.ConflictIgnoreRuleType.Branch => risk.Branch,
                Models.ConflictIgnoreRuleType.Author => string.IsNullOrEmpty(risk.AuthorEmail) ? risk.Author : risk.AuthorEmail,
                Models.ConflictIgnoreRuleType.Pattern => "*" + risk.Extension,
                _ => risk.Path,
            };

            if (string.IsNullOrEmpty(value) || value == "*")
                return;

            var rule = _store.IgnoreRules.Find(x => x.Type == type && x.Value == value);
            if (rule == null)
            {
                rule = new Models.ConflictIgnoreRule() { Type = type, Value = value };
                _store.IgnoreRules.Add(rule);
            }

            foreach (var r in _store.History)
            {
                if (r.Status != Models.ConflictNotificationStatus.Resolved && rule.Matches(r.ToRisk()))
                {
                    r.Status = Models.ConflictNotificationStatus.Ignored;
                    r.StatusAt = DateTime.Now;
                }
            }

            for (var i = Toasts.Count - 1; i >= 0; i--)
            {
                if (rule.Matches(Toasts[i]))
                    Toasts.RemoveAt(i);
            }

            Risks = _risks.FindAll(x => !rule.Matches(x));
            SaveStore();
        }

        public void RemoveIgnoreRule(Models.ConflictIgnoreRule rule)
        {
            _store.IgnoreRules.RemoveAll(x => x.Type == rule.Type && x.Value == rule.Value);
            SaveStore();
            Rescan();
        }

        /// <summary>Bring a dismissed / ignored notification back: drop matching ignore rules and show it again if still relevant.</summary>
        public void Restore(Models.ConflictNotificationRecord record)
        {
            var risk = record.ToRisk();
            _store.IgnoreRules.RemoveAll(x => x.Matches(risk));

            var current = _allRisks.Find(x => x.Key == record.Key);
            var stored = _store.History.FindLast(x => x.Key == record.Key);
            if (current != null && stored != null)
            {
                stored.Status = Models.ConflictNotificationStatus.Active;
                stored.StatusAt = DateTime.Now;
                if (current.IsAlert && !Toasts.Contains(current))
                    Toasts.Insert(0, current);
            }

            SaveStore();
            Rescan();
        }

        public void ClearHistory()
        {
            _store.History.RemoveAll(x => x.Status != Models.ConflictNotificationStatus.Active);
            SaveStore();
        }

        public void Snooze(TimeSpan duration)
        {
            _store.SnoozeUntil = DateTime.Now.Add(duration);
            Toasts.Clear();
            SaveStore();
        }

        public void SnoozeUntilTonight()
        {
            _store.SnoozeUntil = DateTime.Today.AddDays(1).AddSeconds(-1);
            Toasts.Clear();
            SaveStore();
        }

        public void Unsnooze()
        {
            _store.SnoozeUntil = DateTime.MinValue;
            SaveStore();
        }

        public async Task LockFileAsync(Models.ConflictRisk risk)
        {
            var remote = GetRemote();
            if (string.IsNullOrEmpty(remote))
                return;

            Toasts.Remove(risk);
            await new Commands.LFS(_repo.FullPath).LockAsync(remote, risk.Path);
            Rescan();
        }

        public void Dispose()
        {
            _debounce?.Cancel();
            _debounce = null;
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

                // ---- My side: uncommitted files + files in commits not pushed yet.
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

                // ---- LFS: lockable files (local only; the radar does not query locks on the server).
                var usesLFS = UsesLFS(repoPath);
                var lockable = new HashSet<string>(StringComparer.Ordinal);
                if (usesLFS)
                {
                    var paths = new List<string>(mine.Keys);
                    for (var i = 0; i < paths.Count; i += 100)
                    {
                        var chunk = paths.GetRange(i, Math.Min(100, paths.Count - i));
                        lockable.UnionWith(await new Commands.QueryLockableFiles(repoPath, chunk).GetResultAsync().ConfigureAwait(false));
                    }
                }

                token.ThrowIfCancellationRequested();

                var risks = new List<Models.ConflictRisk>();

                // ---- Their side (1): remote branches with a commit in the last N days.
                var since = (ulong)DateTimeOffset.UtcNow.AddDays(-Preferences.Instance.ConflictRadarDays).ToUnixTimeSeconds();
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

                    string mergeBase = null;
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

                        var isBinary = Models.ConflictRisk.LooksBinary(path) ||
                                       (usesLFS && await new Commands.IsLFSFiltered(repoPath, path).GetResultAsync().ConfigureAwait(false));

                        var overlap = true;
                        if (!isBinary)
                        {
                            mergeBase ??= await new Commands.MergeBase(repoPath, "HEAD", b.FullName).GetResultAsync().ConfigureAwait(false);
                            if (!string.IsNullOrEmpty(mergeBase))
                            {
                                var theirRanges = await new Commands.QueryDiffOldRanges(repoPath, $"{mergeBase} {b.FullName}", path).GetResultAsync().ConfigureAwait(false);
                                var myRanges = await new Commands.QueryDiffOldRanges(repoPath, mergeBase, path).GetResultAsync().ConfigureAwait(false);

                                // No hunks on a side (new/untracked or binary-ish file): stay conservative.
                                overlap = theirRanges.Count == 0 || myRanges.Count == 0 || Commands.QueryDiffOldRanges.Overlap(theirRanges, myRanges);
                            }
                        }

                        risks.Add(new Models.ConflictRisk()
                        {
                            Kind = Models.ConflictRiskKind.BranchChange,
                            Level = isBinary ? Models.ConflictRiskLevel.High : (overlap ? Models.ConflictRiskLevel.Medium : Models.ConflictRiskLevel.Low),
                            LinesOverlap = !isBinary && overlap,
                            Path = path,
                            Branch = b.FriendlyName,
                            SHA = t.SHA,
                            Author = t.Author,
                            AuthorEmail = t.Email,
                            Time = t.Time,
                            IsUncommitted = isUncommitted,
                            IsLockable = lockable.Contains(path),
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

                _allRisks = risks;
                var snoozed = IsSnoozed;
                var changed = false;
                var fresh = new List<Models.ConflictRisk>();
                var keys = new HashSet<string>(StringComparer.Ordinal);

                foreach (var r in risks)
                {
                    keys.Add(r.Key);
                    if (IsIgnored(r))
                        continue;

                    var existing = _store.History.FindLast(x => x.Key == r.Key);
                    if (existing != null)
                    {
                        // Came back after being resolved: active again, but no second toast.
                        if (existing.Status == Models.ConflictNotificationStatus.Resolved)
                        {
                            existing.Status = Models.ConflictNotificationStatus.Active;
                            existing.StatusAt = DateTime.Now;
                            changed = true;
                        }

                        continue;
                    }

                    var record = Models.ConflictNotificationRecord.From(r);
                    record.Status = snoozed ? Models.ConflictNotificationStatus.Snoozed : Models.ConflictNotificationStatus.Active;
                    _store.History.Add(record);
                    changed = true;

                    if (!snoozed && r.IsAlert)
                        fresh.Add(r);
                }

                // Risks that went away (pushed, merged, reverted, unlocked...).
                foreach (var h in _store.History)
                {
                    if (h.Status != Models.ConflictNotificationStatus.Resolved && !keys.Contains(h.Key))
                    {
                        h.Status = Models.ConflictNotificationStatus.Resolved;
                        h.StatusAt = DateTime.Now;
                        changed = true;
                    }
                }

                var visible = risks.FindAll(x => !IsIgnored(x));

                for (var i = Toasts.Count - 1; i >= 0; i--)
                {
                    if (!visible.Exists(x => x.Key == Toasts[i].Key))
                        Toasts.RemoveAt(i);
                }

                Risks = visible;

                foreach (var r in fresh)
                {
                    Toasts.Insert(0, r);
                    while (Toasts.Count > MaxToasts)
                        Toasts.RemoveAt(Toasts.Count - 1);
                }

                if (changed)
                    SaveStore();

                if (fresh.Count > 0)
                    AlertRaised?.Invoke(this, fresh, fresh.Exists(x => x.IsLoud));

                OnPropertyChanged(nameof(IsSnoozed));
                OnPropertyChanged(nameof(SnoozeText));
            });
        }

        private bool IsIgnored(Models.ConflictRisk risk)
        {
            foreach (var rule in _store.IgnoreRules)
            {
                if (rule.Matches(risk))
                    return true;
            }

            return false;
        }

        private void SetStatus(string key, Models.ConflictNotificationStatus status)
        {
            var record = _store.History.FindLast(x => x.Key == key);
            if (record != null)
            {
                record.Status = status;
                record.StatusAt = DateTime.Now;
            }
        }

        private void SaveStore()
        {
            _store.Save(_repo.GitDir);
            OnPropertyChanged(nameof(History));
            OnPropertyChanged(nameof(HasHistory));
            OnPropertyChanged(nameof(IgnoreRules));
            OnPropertyChanged(nameof(HasIgnoreRules));
            OnPropertyChanged(nameof(IsSnoozed));
            OnPropertyChanged(nameof(SnoozeText));
        }

        private void ClearAll()
        {
            _debounce?.Cancel();
            Toasts.Clear();
            Risks = [];
        }

        private string GetRemote()
        {
            var remotes = _repo.Remotes;
            if (remotes == null || remotes.Count == 0)
                return null;

            return remotes.Find(x => x.Name == "origin")?.Name ?? remotes[0].Name;
        }

        private static bool UsesLFS(string repoPath)
        {
            try
            {
                var attrs = Path.Combine(repoPath, ".gitattributes");
                return File.Exists(attrs) && File.ReadAllText(attrs).Contains("filter=lfs", StringComparison.Ordinal);
            }
            catch
            {
                return false;
            }
        }


        private const int MaxToasts = 3;

        private readonly Repository _repo;
        private readonly Models.ConflictRadarStore _store;
        private List<Models.Branch> _branches = null;
        private HashSet<string> _localPaths = [];
        private List<Models.ConflictRisk> _risks = [];
        private List<Models.ConflictRisk> _allRisks = [];
        private bool _isScanning = false;
        private string _myEmail = null;
        private CancellationTokenSource _debounce = null;
        private readonly ConcurrentDictionary<string, HashSet<string>> _fileCache = new(StringComparer.Ordinal);
    }
}
