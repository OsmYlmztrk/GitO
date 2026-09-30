using System.IO;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SourceGit.ViewModels
{
    public class Conflict : ObservableObject
    {
        public string Marker
        {
            get => _change.ConflictMarker;
        }

        public string Description
        {
            get => _change.ConflictDesc;
        }

        public Models.ConflictFileState State
        {
            get => _state;
            private set => SetProperty(ref _state, value);
        }

        public object Theirs
        {
            get => _theirs;
            private set => SetProperty(ref _theirs, value);
        }

        public object Mine
        {
            get => _mine;
            private set => SetProperty(ref _mine, value);
        }

        // MyGitGUI: extra info for the Fork-style conflict panel (binary assets, LFS locks)
        public string FilePath
        {
            get => _change.Path;
        }

        public string FileName
        {
            get => Path.GetFileName(_change.Path);
        }

        public bool IsBinary
        {
            get => _state == Models.ConflictFileState.UnmergedBinary;
        }

        public bool CanUseExternalMergeTool
        {
            get => _state is Models.ConflictFileState.UnmergedText or Models.ConflictFileState.UnmergedBinary;
        }

        public bool IsLFS
        {
            get => _isLFS;
            private set => SetProperty(ref _isLFS, value);
        }

        public bool IsLockInfoLoading
        {
            get => _isLockInfoLoading;
            private set => SetProperty(ref _isLockInfoLoading, value);
        }

        public Models.LFSLock Lock
        {
            get => _lock;
            private set
            {
                if (SetProperty(ref _lock, value))
                {
                    OnPropertyChanged(nameof(IsLocked));
                    OnPropertyChanged(nameof(IsLockedByMe));
                    OnPropertyChanged(nameof(IsLockedByOthers));
                }
            }
        }

        public bool IsLocked
        {
            get => _lock != null;
        }

        public bool IsLockedByMe
        {
            get => _lock != null && !string.IsNullOrEmpty(_userName) && _lock.Owner?.Name == _userName;
        }

        public bool IsLockedByOthers
        {
            get => _lock != null && !IsLockedByMe;
        }

        public bool HasRemote
        {
            get => !string.IsNullOrEmpty(_remote);
        }

        public Conflict(Repository repo, WorkingCopy wc, Models.Change change)
        {
            _repo = repo;
            _wc = wc;
            _change = change;
            _remote = repo.Remotes.Find(x => x.Name == "origin")?.Name ?? (repo.Remotes.Count > 0 ? repo.Remotes[0].Name : null);

            Task.Run(async () =>
            {
                _head = new Commands.QuerySingleCommit(repo.FullPath, "HEAD").GetResult();

                var (mine, theirs) = wc.InProgressContext switch
                {
                    CherryPickInProgress cherryPick => (_head, cherryPick.Head),
                    RebaseInProgress rebase => (rebase.Onto, rebase.StoppedAt),
                    RevertInProgress revert => (_head, revert.Head),
                    MergeInProgress merge => (_head, merge.Source),
                    _ => (_head, (object)"Stash or Patch"),
                };

                var state = Models.ConflictFileState.Unknown;
                if ((_change.ConflictReason is Models.ConflictReason.BothAdded or Models.ConflictReason.BothModified) && !Directory.Exists(Path.Combine(_repo.FullPath, _change.Path)))
                    state = await new Commands.QueryConflictFileState(repo.FullPath, change)
                        .GetResultAsync()
                        .ConfigureAwait(false);

                var isLFS = await new Commands.IsLFSFiltered(repo.FullPath, change.Path)
                    .GetResultAsync()
                    .ConfigureAwait(false);

                Dispatcher.UIThread.Post(() =>
                {
                    State = state;
                    Mine = mine;
                    Theirs = theirs;
                    IsLFS = isLFS;
                    OnPropertyChanged(nameof(IsBinary));
                    OnPropertyChanged(nameof(CanUseExternalMergeTool));
                });

                if (isLFS)
                    await RefreshLockAsync().ConfigureAwait(false);
            });
        }

        public async Task RefreshLockAsync()
        {
            if (string.IsNullOrEmpty(_remote))
                return;

            Dispatcher.UIThread.Post(() => IsLockInfoLoading = true);

            _userName = await new Commands.Config(_repo.FullPath).GetAsync("user.name").ConfigureAwait(false);
            var locks = await new Commands.LFS(_repo.FullPath).GetLocksAsync(_remote).ConfigureAwait(false);
            var found = locks.Find(x => x.Path == _change.Path);

            Dispatcher.UIThread.Post(() =>
            {
                Lock = found;
                IsLockInfoLoading = false;
            });
        }

        public async Task LockAsync()
        {
            if (string.IsNullOrEmpty(_remote))
                return;

            await new Commands.LFS(_repo.FullPath).LockAsync(_remote, _change.Path);
            await RefreshLockAsync();
        }

        public async Task UnlockAsync(bool force)
        {
            if (string.IsNullOrEmpty(_remote) || _lock == null)
                return;

            await new Commands.LFS(_repo.FullPath).UnlockAsync(_remote, _change.Path, force);
            await RefreshLockAsync();
        }

        public async Task UseTheirsAsync()
        {
            await _wc.UseTheirsAsync([_change]);
        }

        public async Task UseMineAsync()
        {
            await _wc.UseMineAsync([_change]);
        }

        public MergeConflictEditor CreateOpenMergeEditorRequest()
        {
            return _state == Models.ConflictFileState.UnmergedText ? new MergeConflictEditor(_repo, _head, _change.Path) : null;
        }

        public async Task MergeExternalAsync()
        {
            if (CanUseExternalMergeTool)
                await _wc.UseExternalMergeToolAsync(_change);
        }

        private Repository _repo = null;
        private WorkingCopy _wc = null;
        private Models.Change _change = null;
        private Models.Commit _head = null;
        private Models.ConflictFileState _state = Models.ConflictFileState.Unknown;
        private object _mine = null;
        private object _theirs = null;
        private string _remote = null;
        private string _userName = null;
        private bool _isLFS = false;
        private bool _isLockInfoLoading = false;
        private Models.LFSLock _lock = null;
    }
}
