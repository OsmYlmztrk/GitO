using System;
using System.Collections.Generic;
using System.Diagnostics;
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

        // MyGitGUI: extra info for the Fork-style conflict panel (binary assets)
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

        // MyGitGUI: Unreal assets can be compared / merged in Unreal Editor (-diff command line).
        public bool IsUnrealAsset
        {
            get => !string.IsNullOrEmpty(_unrealProject);
        }

        public bool CanOpenInUnreal
        {
            get => !string.IsNullOrEmpty(_unrealEditor) && !_isUnrealBusy;
        }

        public bool CanMergeInUnreal
        {
            get => CanOpenInUnreal && _change.ConflictReason == Models.ConflictReason.BothModified;
        }

        public bool IsUnrealBusy
        {
            get => _isUnrealBusy;
            private set
            {
                if (SetProperty(ref _isUnrealBusy, value))
                {
                    OnPropertyChanged(nameof(CanOpenInUnreal));
                    OnPropertyChanged(nameof(CanMergeInUnreal));
                }
            }
        }

        public string UnrealStatus
        {
            get => _unrealStatus;
            private set => SetProperty(ref _unrealStatus, value);
        }

        public bool IsUnrealStatusError
        {
            get => _isUnrealStatusError;
            private set => SetProperty(ref _isUnrealStatusError, value);
        }

        // MyGitGUI: our GitBlueprintMerge plugin is installed for this project's engine version.
        public bool HasMergePlugin
        {
            get => !string.IsNullOrEmpty(_mergePlugin);
        }

        public List<Models.BlueprintMergeChange> UnrealMergeChanges
        {
            get => _unrealMergeChanges;
            private set => SetProperty(ref _unrealMergeChanges, value);
        }

        /// <summary>
        ///     Offered after our automatic merge could not finish, so the user can still open Unreal's own merge window.
        /// </summary>
        public bool CanOpenUnrealMergeTool
        {
            get => _canOpenUnrealMergeTool;
            private set => SetProperty(ref _canOpenUnrealMergeTool, value);
        }

        /// <summary>
        ///     Offered when our automatic merge found conflicts: they are decided in our merge window in Unreal.
        /// </summary>
        public bool CanResolveInUnreal
        {
            get => _canResolveInUnreal;
            private set => SetProperty(ref _canResolveInUnreal, value);
        }

        // MyGitGUI: levels are merged only when both sides keep each actor in its own file (One File Per Actor).
        public string LevelNotice
        {
            get => _levelNotice;
            private set => SetProperty(ref _levelNotice, value);
        }

        public Conflict(Repository repo, WorkingCopy wc, Models.Change change)
        {
            _repo = repo;
            _wc = wc;
            _change = change;

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

                string unrealProject = null, unrealEditor = null, mergePlugin = null;
                if (Models.UnrealEngine.IsAssetFile(change.Path))
                {
                    unrealProject = Models.UnrealEngine.FindProject(Path.Combine(repo.FullPath, change.Path));
                    unrealEditor = Models.UnrealEngine.FindEditor(unrealProject);
                    mergePlugin = Models.UnrealEngine.FindMergePlugin(unrealProject);
                }

                string levelNotice = null;
                if (unrealProject != null && Models.UnrealLevel.IsLevelFile(change.Path))
                    levelNotice = await GetLevelNoticeAsync(mine as Models.Commit, theirs as Models.Commit).ConfigureAwait(false);

                // Names for the two sides in the merge window: "main (cdc13fb)", "theirs-branch (65aaab4)".
                var oursLabel = SideLabel(wc.InProgressContext is RebaseInProgress ? null : repo.CurrentBranch?.Name, mine as Models.Commit);
                var theirsLabel = wc.InProgressContext is MergeInProgress inMerge
                    ? SideLabel(inMerge.SourceName, inMerge.Source)
                    : SideLabel(null, theirs as Models.Commit);

                Dispatcher.UIThread.Post(() =>
                {
                    State = state;
                    Mine = mine;
                    Theirs = theirs;
                    IsLFS = isLFS;
                    _unrealProject = unrealProject;
                    _unrealEditor = unrealEditor;
                    _mergePlugin = mergePlugin;
                    _oursLabel = oursLabel;
                    _theirsLabel = theirsLabel;
                    LevelNotice = levelNotice;
                    if (unrealProject != null && unrealEditor == null)
                        ShowUnrealError("Unreal Editor for this project's engine version was not found.");
                    OnPropertyChanged(nameof(HasMergePlugin));
                    OnPropertyChanged(nameof(IsBinary));
                    OnPropertyChanged(nameof(CanUseExternalMergeTool));
                    OnPropertyChanged(nameof(IsUnrealAsset));
                    OnPropertyChanged(nameof(CanOpenInUnreal));
                    OnPropertyChanged(nameof(CanMergeInUnreal));
                });
            });
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

        /// <summary>
        ///     Opens Unreal's diff window with Ours (left) and Theirs (right). The app stays usable meanwhile.
        /// </summary>
        public async Task CompareInUnrealAsync()
        {
            if (!CanOpenInUnreal)
                return;

            IsUnrealBusy = true;
            IsUnrealStatusError = false;
            UnrealStatus = "Opening Unreal's diff: Ours on the left, Theirs on the right. Unreal can take a moment to start; close it when you're done.";

            var tempDir = Path.Combine(Path.GetTempPath(), "MyGitGUI", "Unreal", Guid.NewGuid().ToString("N"));
            try
            {
                var stages = await new Commands.CheckoutConflictStages(_repo.FullPath, _change.Path).GetResultAsync();
                if (stages.Ours == null || stages.Theirs == null)
                {
                    DeleteFiles(stages.Base, stages.Ours, stages.Theirs);
                    ShowUnrealError("Couldn't read the conflicting versions of this file.");
                    return;
                }

                // Unreal shows the text after '#' in the file name as the revision label.
                Directory.CreateDirectory(tempDir);
                var name = Path.GetFileNameWithoutExtension(_change.Path);
                var ext = Path.GetExtension(_change.Path);
                var ours = MoveTo(stages.Ours, Path.Combine(tempDir, $"{name}#Ours{ext}"));
                var theirs = MoveTo(stages.Theirs, Path.Combine(tempDir, $"{name}#Theirs{ext}"));
                DeleteFiles(stages.Base);

                using var proc = Process.Start(new ProcessStartInfo(_unrealEditor, $"{_unrealProject.Quoted()} -diff {ours.Quoted()} {theirs.Quoted()}") { UseShellExecute = false });
                if (proc == null)
                {
                    ShowUnrealError("Couldn't start Unreal Editor.");
                    return;
                }

                await proc.WaitForExitAsync();
                UnrealStatus = string.Empty;
            }
            catch (Exception e)
            {
                ShowUnrealError(e.Message);
            }
            finally
            {
                IsUnrealBusy = false;
                try
                {
                    if (Directory.Exists(tempDir))
                        Directory.Delete(tempDir, true);
                }
                catch
                {
                    // Unreal works on its own copies; a leftover temp folder is harmless.
                }
            }
        }

        /// <summary>
        ///     Merges with our GitBlueprintMerge plugin when it is installed (automatic, conflicts decided in Unreal),
        ///     otherwise in Unreal's own merge window. Runs as a progress popup over the repository page.
        /// </summary>
        public Task MergeInUnrealAsync()
        {
            return StartUnrealMergeAsync(HasMergePlugin ? UnrealMerge.Mode.Auto : UnrealMerge.Mode.UnrealWindow);
        }

        /// <summary>
        ///     Opens our merge window in Unreal right away: both graphs side by side, Keep ours / Take theirs per conflict.
        /// </summary>
        public Task ResolveInUnrealAsync()
        {
            return StartUnrealMergeAsync(UnrealMerge.Mode.Resolve);
        }

        /// <summary>
        ///     Unreal's own merge window (keeps one side), for assets our plugin can't merge.
        /// </summary>
        public Task OpenUnrealMergeToolAsync()
        {
            return StartUnrealMergeAsync(UnrealMerge.Mode.UnrealWindow);
        }

        /// <summary>
        ///     Called by the merge popup when it ends: what happened and what can be done next.
        /// </summary>
        public void ShowUnrealOutcome(UnrealMergeOutcome outcome)
        {
            UnrealStatus = outcome.Message;
            IsUnrealStatusError = outcome.IsError;
            UnrealMergeChanges = outcome.Changes;
            CanResolveInUnreal = outcome.CanResolve && HasMergePlugin;
            CanOpenUnrealMergeTool = outcome.CanUseUnrealWindow;
        }

        private async Task StartUnrealMergeAsync(UnrealMerge.Mode mode)
        {
            if (!CanMergeInUnreal || !_repo.CanCreatePopup())
                return;

            UnrealStatus = string.Empty;
            IsUnrealStatusError = false;
            UnrealMergeChanges = null;
            CanResolveInUnreal = false;
            CanOpenUnrealMergeTool = false;

            var target = new UnrealMergeTarget()
            {
                Path = _change.Path,
                Project = _unrealProject,
                Editor = _unrealEditor,
                Plugin = _mergePlugin,
                OursLabel = _oursLabel,
                TheirsLabel = _theirsLabel,
            };

            await _repo.ShowAndStartPopupAsync(new UnrealMerge(_repo, this, target, mode));
        }

        private void ShowUnrealError(string message)
        {
            UnrealStatus = message;
            IsUnrealStatusError = true;
        }

        /// <summary>
        ///     Null when both sides use One File Per Actor. A stash or patch has no commit to check,
        ///     so that side counts as the same layout as ours.
        /// </summary>
        private async Task<string> GetLevelNoticeAsync(Models.Commit ours, Models.Commit theirs)
        {
            var oursSplit = ours != null
                ? await Models.UnrealLevel.UsesOneFilePerActorAsync(_repo.FullPath, ours.SHA, _change.Path).ConfigureAwait(false)
                : Models.UnrealLevel.UsesOneFilePerActor(_repo.FullPath, _change.Path);
            var theirsSplit = theirs != null
                ? await Models.UnrealLevel.UsesOneFilePerActorAsync(_repo.FullPath, theirs.SHA, _change.Path).ConfigureAwait(false)
                : oursSplit;

            if (oursSplit && theirsSplit)
                return null;

            if (!oursSplit && !theirsSplit)
                return "This level keeps all its actors in one file, so GitO can't merge it change by change. " +
                       "Keep one side and redo the other side's changes in Unreal. " +
                       "To stop this from happening again, turn on One File Per Actor for this level: each actor gets its own file, " +
                       "and people editing different actors no longer conflict.";

            var converted = oursSplit ? "Ours" : "Theirs";
            var other = oursSplit ? "Theirs" : "Ours";
            return $"One File Per Actor was turned on for this level in {converted} only. GitO merges a level only when both sides use it. " +
                   $"Take {converted} (its actors are in their own files) and redo {other}' changes to this level in Unreal.";
        }

        /// <summary>
        ///     "main (cdc13fb)": the branch name when known, plus the short commit hash.
        /// </summary>
        private static string SideLabel(string branch, Models.Commit commit)
        {
            var sha = commit is { SHA.Length: >= 7 } ? commit.SHA.Substring(0, 7) : null;
            if (string.IsNullOrEmpty(branch))
                return sha ?? string.Empty;
            return sha != null ? $"{branch} ({sha})" : branch;
        }

        private static string MoveTo(string src, string dst)
        {
            if (src == null)
                return null;

            File.Move(src, dst, true);
            return dst;
        }

        private static void DeleteFiles(params string[] files)
        {
            foreach (var file in files)
            {
                if (file != null && File.Exists(file))
                    File.Delete(file);
            }
        }

        private Repository _repo = null;
        private WorkingCopy _wc = null;
        private Models.Change _change = null;
        private Models.Commit _head = null;
        private Models.ConflictFileState _state = Models.ConflictFileState.Unknown;
        private object _mine = null;
        private object _theirs = null;
        private bool _isLFS = false;
        private string _unrealProject = null;
        private string _unrealEditor = null;
        private string _unrealStatus = string.Empty;
        private bool _isUnrealBusy = false;
        private string _mergePlugin = null;
        private List<Models.BlueprintMergeChange> _unrealMergeChanges = null;
        private bool _canOpenUnrealMergeTool = false;
        private bool _canResolveInUnreal = false;
        private bool _isUnrealStatusError = false;
        private string _oursLabel = string.Empty;
        private string _theirsLabel = string.Empty;
        private string _levelNotice = null;
    }
}
