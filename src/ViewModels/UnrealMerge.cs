using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;

namespace SourceGit.ViewModels
{
    public enum UnrealMergeStepState
    {
        Pending,
        Running,
        Done,
        Skipped,
        Failed,
    }

    public class UnrealMergeStep : ObservableObject
    {
        public string Title
        {
            get => _title;
            set => SetProperty(ref _title, value);
        }

        public string Detail
        {
            get => _detail;
            set => SetProperty(ref _detail, value);
        }

        public string Time
        {
            get => _time;
            set => SetProperty(ref _time, value);
        }

        public UnrealMergeStepState State
        {
            get => _state;
            set
            {
                if (SetProperty(ref _state, value))
                {
                    OnPropertyChanged(nameof(IsPending));
                    OnPropertyChanged(nameof(IsRunning));
                    OnPropertyChanged(nameof(IsDone));
                    OnPropertyChanged(nameof(IsSkipped));
                    OnPropertyChanged(nameof(IsFailed));
                }
            }
        }

        public bool IsPending => _state == UnrealMergeStepState.Pending;
        public bool IsRunning => _state == UnrealMergeStepState.Running;
        public bool IsDone => _state == UnrealMergeStepState.Done;
        public bool IsSkipped => _state == UnrealMergeStepState.Skipped;
        public bool IsFailed => _state == UnrealMergeStepState.Failed;

        public DateTime StartedAt { get; set; }

        public UnrealMergeStep(string title)
        {
            _title = title;
        }

        private string _title;
        private string _detail = string.Empty;
        private string _time = string.Empty;
        private UnrealMergeStepState _state = UnrealMergeStepState.Pending;
    }

    /// <summary>
    ///     What the merge of one conflicted Unreal asset needs to know.
    /// </summary>
    public class UnrealMergeTarget
    {
        public string Path { get; set; }
        public string Project { get; set; }
        public string Editor { get; set; }
        public string Plugin { get; set; }
        public string OursLabel { get; set; }
        public string TheirsLabel { get; set; }
    }

    /// <summary>
    ///     How a merge ended, for the conflict panel.
    /// </summary>
    public class UnrealMergeOutcome
    {
        public bool Staged { get; set; }
        public bool IsError { get; set; }
        public bool CanResolve { get; set; }
        public bool CanUseUnrealWindow { get; set; }
        public string Message { get; set; } = string.Empty;
        public List<Models.BlueprintMergeChange> Changes { get; set; }
    }

    /// <summary>
    ///     MyGitGUI: merges a conflicted Unreal asset step by step while the repository page waits: read the three
    ///     versions, let our GitBlueprintMerge plugin combine them in the background, open its merge window in Unreal
    ///     when there are conflicts, then save and stage the result. Ends with a notification.
    /// </summary>
    public class UnrealMerge : Popup
    {
        public enum Mode
        {
            /// <summary>Merge in the background; open our merge window only for conflicts.</summary>
            Auto,
            /// <summary>Open our merge window right away.</summary>
            Resolve,
            /// <summary>Unreal's own merge window (keeps one side), for assets our plugin can't read.</summary>
            UnrealWindow,
        }

        public string Title { get; }
        public string OursLabel { get; }
        public string TheirsLabel { get; }
        public List<UnrealMergeStep> Steps { get; } = [];

        public string Elapsed
        {
            get => _elapsed;
            private set => SetProperty(ref _elapsed, value);
        }

        public string Hint
        {
            get => _hint;
            private set => SetProperty(ref _hint, value);
        }

        public UnrealMerge(Repository repo, Conflict owner, UnrealMergeTarget target, Mode mode)
        {
            _repo = repo;
            _owner = owner;
            _target = target;
            _mode = mode;
            _assetName = System.IO.Path.GetFileName(target.Path);

            Title = $"Merge {_assetName}";
            OursLabel = string.IsNullOrEmpty(target.OursLabel) ? "your branch" : target.OursLabel;
            TheirsLabel = string.IsNullOrEmpty(target.TheirsLabel) ? "incoming" : target.TheirsLabel;
            CanTerminate = true;

            _read = new UnrealMergeStep("Get Base, Ours and Theirs from Git");
            _analyze = new UnrealMergeStep("Combine both sides (Unreal, in the background)");
            _decide = new UnrealMergeStep(mode == Mode.UnrealWindow ? "Merge in Unreal's merge window" : "Decide the conflicts in Unreal");
            _save = new UnrealMergeStep("Save and stage the result");

            Steps.Add(_read);
            if (mode == Mode.Auto)
                Steps.Add(_analyze);
            Steps.Add(_decide);
            Steps.Add(_save);
        }

        public override async Task<bool> Sure()
        {
            _startedAt = DateTime.Now;
            var timer = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => Tick());
            timer.Start();

            var tempDir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "MyGitGUI", "Unreal", Guid.NewGuid().ToString("N"));
            UnrealMergeOutcome outcome;
            try
            {
                outcome = await RunAsync(tempDir);
            }
            catch (Exception e)
            {
                outcome = new UnrealMergeOutcome() { IsError = true, CanUseUnrealWindow = true, Message = $"Couldn't merge {_assetName}: {e.Message}" };
            }
            finally
            {
                timer.Stop();
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

            if (outcome.Staged || outcome.IsError)
            {
                _repo.SendNotification(outcome.Message, outcome.IsError);
                App.RequestAttention();
            }

            if (outcome.Staged)
                _repo.MarkWorkingCopyDirtyManually();

            _owner?.ShowUnrealOutcome(outcome);
            return true;
        }

        public override void Terminate()
        {
            _terminated = true;
            try
            {
                if (_process is { HasExited: false })
                    _process.Kill(true);
            }
            catch
            {
                // Already gone.
            }
        }

        private async Task<UnrealMergeOutcome> RunAsync(string tempDir)
        {
            // 1. The three versions, with real content (LFS is resolved by git's filters).
            Begin(_read, "Reading the conflicting versions from Git...");
            var stages = await new Commands.CheckoutConflictStages(_repo.FullPath, _target.Path).GetResultAsync();
            if (stages.Ours == null || stages.Theirs == null || stages.Base == null)
            {
                DeleteFiles(stages.Base, stages.Ours, stages.Theirs);
                End(_read, UnrealMergeStepState.Failed, "Missing a version (both added?)");
                return Error($"Couldn't read the conflicting versions of {_assetName}.");
            }

            Directory.CreateDirectory(tempDir);
            var name = System.IO.Path.GetFileNameWithoutExtension(_target.Path);
            var ext = System.IO.Path.GetExtension(_target.Path);
            var files = new Revisions()
            {
                // Unreal shows the text after '#' in the file name as the revision label.
                Base = MoveTo(stages.Base, System.IO.Path.Combine(tempDir, $"{name}#Base{ext}")),
                Ours = MoveTo(stages.Ours, System.IO.Path.Combine(tempDir, $"{name}#Ours{ext}")),
                Theirs = MoveTo(stages.Theirs, System.IO.Path.Combine(tempDir, $"{name}#Theirs{ext}")),
                Merged = System.IO.Path.Combine(tempDir, "Merged" + ext),
                Report = System.IO.Path.Combine(tempDir, "report.json"),
                Result = System.IO.Path.Combine(_repo.FullPath, _target.Path),
            };
            End(_read, UnrealMergeStepState.Done, string.Empty);

            if (_mode == Mode.UnrealWindow)
                return await RunUnrealWindowAsync(files);

            // 2. Combine in the background; only conflicts need a person.
            if (_mode == Mode.Auto)
            {
                Begin(_analyze, "Unreal is loading your project in the background (no window). The first run can take a minute.");
                var report = await RunPluginAsync(files, false);
                if (_terminated)
                    return Cancelled();

                if (report == null)
                {
                    End(_analyze, UnrealMergeStepState.Failed, "This asset type can't be merged automatically.");
                    _decide.Title = "Merge in Unreal's merge window";
                    return await RunUnrealWindowAsync(files);
                }

                var total = report.Summary.Ours + report.Summary.Theirs + report.Summary.Both;
                var analysis = report.Conflicts.Count > 0
                    ? $"{total} change(s) combined, {report.Conflicts.Count} conflict(s) need you"
                    : $"{total} change(s) combined, no conflicts";
                if (report.Unsupported.Count > 0)
                    analysis += $", {report.Unsupported.Count} can't be merged automatically";
                End(_analyze, UnrealMergeStepState.Done, analysis);

                if (report.Written)
                {
                    End(_decide, UnrealMergeStepState.Skipped, "No conflicts");
                    return await StageAsync(files, report);
                }

                // Changes the plugin can't merge yet: the merge window lists them and lets the person leave them out.
                if (report.Problems.Count > 0 && report.Unsupported.Count == 0)
                {
                    End(_decide, UnrealMergeStepState.Skipped, string.Empty);
                    return Error($"Couldn't merge {_assetName}: {string.Join(" ", report.Problems)}", report);
                }
            }

            // 3. Our merge window in Unreal.
            Begin(_decide, $"Switch to Unreal: the \"Merge {name}\" window shows both versions side by side. " +
                "Pick Keep ours or Take theirs for each conflict, then press Merge. Closing the window cancels without changes.");
            var decided = await RunPluginAsync(files, true);
            if (_terminated)
                return Cancelled();

            if (decided == null)
            {
                End(_decide, UnrealMergeStepState.Failed, "Unreal closed unexpectedly");
                return Error($"Unreal closed before the merge finished (log: {LogFile}).");
            }

            if (!decided.Written)
            {
                if (decided.Problems.Count > 0)
                {
                    End(_decide, UnrealMergeStepState.Failed, string.Join(" ", decided.Problems));
                    return Error($"Couldn't merge {_assetName}: {string.Join(" ", decided.Problems)}", decided);
                }

                End(_decide, UnrealMergeStepState.Skipped, "Closed without merging");
                return new UnrealMergeOutcome()
                {
                    CanResolve = true,
                    Changes = ListChanges(decided),
                    Message = $"The merge window was closed without merging; nothing changed. {decided.Conflicts.Count} conflict(s) still need a decision: " +
                        "use Resolve in Unreal, or keep one side with Take Ours / Take Theirs.",
                };
            }

            End(_decide, UnrealMergeStepState.Done, $"{decided.Summary.Resolved} decision(s)");
            return await StageAsync(files, decided);
        }

        /// <summary>
        ///     Our GitBlueprintMerge plugin: as a commandlet (no window) or as the merge window. Null when Unreal wrote
        ///     no report, i.e. the plugin couldn't read the asset or Unreal stopped early.
        /// </summary>
        private async Task<Models.BlueprintMergeReport> RunPluginAsync(Revisions files, bool interactive)
        {
            var exe = interactive ? _target.Editor : System.IO.Path.Combine(System.IO.Path.GetDirectoryName(_target.Editor)!, "UnrealEditor-Cmd.exe");
            if (!File.Exists(exe))
                return null;

            if (File.Exists(files.Report))
                File.Delete(files.Report);

            var packageName = Models.UnrealEngine.GetPackageName(_target.Project, files.Result);
            var args = new StringBuilder();
            args.Append(_target.Project.Quoted())
                .Append(interactive ? " -GitBlueprintMergeUI" : " -run=GitBlueprintMerge")
                .Append($" -PLUGIN=\"{_target.Plugin}\" -EnablePlugins=GitBlueprintMerge")
                .Append($" -Base=\"{files.Base}\" -Ours=\"{files.Ours}\" -Theirs=\"{files.Theirs}\"")
                .Append($" -Report=\"{files.Report}\" -Output=\"{files.Merged}\"");
            if (!string.IsNullOrEmpty(packageName))
                args.Append($" -PackageName=\"{packageName}\"");
            if (interactive)
                args.Append($" -OursLabel=\"{OursLabel}\" -TheirsLabel=\"{TheirsLabel}\"");
            args.Append(interactive ? " -nopause" : " -unattended -nullrhi -nosplash -nopause");
            args.Append($" -abslog=\"{LogFile}\"");

            _process = Process.Start(new ProcessStartInfo(exe, args.ToString()) { UseShellExecute = false, CreateNoWindow = !interactive });
            if (_process == null)
                return null;

            using (_process)
            {
                await _process.WaitForExitAsync();
            }
            _process = null;

            // The exit code also counts unrelated project errors; the report is the source of truth.
            if (!File.Exists(files.Report))
                return null;

            await using var stream = File.OpenRead(files.Report);
            return await JsonSerializer.DeserializeAsync(stream, JsonCodeGen.Default.BlueprintMergeReport);
        }

        /// <summary>
        ///     Unreal's own merge window: shows the three versions, can keep only one side, saves into the working copy.
        /// </summary>
        private async Task<UnrealMergeOutcome> RunUnrealWindowAsync(Revisions files)
        {
            Begin(_decide, "Unreal's merge window is opening. It can only keep one side: choose Accept Source (theirs) or " +
                "Accept Target (ours), then Finish Merge.");

            var before = GetFileStamp(files.Result);
            var args = $"{_target.Project.Quoted()} -diff {files.Theirs.Quoted()} {files.Ours.Quoted()} {files.Base.Quoted()} {files.Result.Quoted()}";
            _process = Process.Start(new ProcessStartInfo(_target.Editor, args) { UseShellExecute = false });
            if (_process == null)
            {
                End(_decide, UnrealMergeStepState.Failed, "Could not start Unreal");
                return Error("Could not start Unreal Editor.");
            }

            using (_process)
            {
                await _process.WaitForExitAsync();
            }
            _process = null;

            if (_terminated)
                return Cancelled();

            if (GetFileStamp(files.Result) == before)
            {
                End(_decide, UnrealMergeStepState.Skipped, "Closed without saving");
                return new UnrealMergeOutcome() { Message = "Unreal's merge window was closed without saving; nothing changed." };
            }

            End(_decide, UnrealMergeStepState.Done, string.Empty);
            Begin(_save, "Staging the result...");
            await StageFileAsync();
            End(_save, UnrealMergeStepState.Done, string.Empty);
            return new UnrealMergeOutcome()
            {
                Staged = true,
                Message = $"{_assetName} merged in Unreal's merge window and staged. Open it in Unreal to check it before you commit.",
            };
        }

        private async Task<UnrealMergeOutcome> StageAsync(Revisions files, Models.BlueprintMergeReport report)
        {
            Begin(_save, "Writing the merged asset into your working copy and staging it...");
            File.Copy(files.Merged, files.Result, true);
            await StageFileAsync();
            End(_save, UnrealMergeStepState.Done, string.Empty);

            var summary = report.Summary;
            var automatic = summary.Ours + summary.Theirs + summary.Both;
            var message = new StringBuilder();
            message.Append(_assetName).Append(" merged and staged.");
            if (summary.Resolved > 0)
                message.Append($"\n- You decided {summary.Resolved} conflict(s).");
            message.Append($"\n- {automatic} change(s) combined automatically ({summary.Ours} from {OursLabel}, {summary.Theirs} from {TheirsLabel}, {summary.Both} on both).");
            if (report.LeftOut.Count > 0)
            {
                message.Append($"\n- Left out {report.LeftOut.Count} change(s) from {TheirsLabel}; redo them by hand in Unreal before you commit:");
                foreach (var item in report.LeftOut)
                    message.Append("\n    ").Append(item);
            }
            message.Append("\nOpen it in Unreal to check it before you commit.");

            return new UnrealMergeOutcome()
            {
                Staged = true,
                Changes = ListChanges(report),
                Message = message.ToString(),
            };
        }

        private async Task StageFileAsync()
        {
            var log = _repo.CreateLog("Merge Unreal asset");
            var pathSpecFile = System.IO.Path.GetTempFileName();
            await File.WriteAllLinesAsync(pathSpecFile, [_target.Path]);
            await new Commands.Add(_repo.FullPath, pathSpecFile).Use(log).ExecAsync();
            File.Delete(pathSpecFile);
            log.Complete();
        }

        private UnrealMergeOutcome Error(string message, Models.BlueprintMergeReport report = null)
        {
            return new UnrealMergeOutcome()
            {
                IsError = true,
                CanUseUnrealWindow = true,
                Changes = report != null ? ListChanges(report) : null,
                Message = message,
            };
        }

        private UnrealMergeOutcome Cancelled()
        {
            foreach (var step in Steps)
            {
                if (step.IsRunning)
                    End(step, UnrealMergeStepState.Skipped, "Cancelled");
            }

            return new UnrealMergeOutcome() { CanResolve = _mode != Mode.UnrealWindow, Message = "Merge cancelled; nothing changed." };
        }

        /// <summary>
        ///     One line per open conflict, then what merged on its own.
        /// </summary>
        private static List<Models.BlueprintMergeChange> ListChanges(Models.BlueprintMergeReport report)
        {
            var changes = new List<Models.BlueprintMergeChange>();
            if (!report.Written)
            {
                foreach (var conflict in report.Conflicts)
                    changes.Add(new Models.BlueprintMergeChange() { Resolution = "conflict", Text = conflict.Text });
            }

            changes.AddRange(report.Changes.FindAll(x => !x.IsConflict));
            return changes;
        }

        private void Begin(UnrealMergeStep step, string hint)
        {
            step.State = UnrealMergeStepState.Running;
            step.StartedAt = DateTime.Now;
            step.Time = "0:00";
            Hint = hint;
            ProgressDescription = $"{step.Title}...";
        }

        private void End(UnrealMergeStep step, UnrealMergeStepState state, string detail)
        {
            if (step.IsRunning)
                step.Time = FormatTime(DateTime.Now - step.StartedAt);
            else
                step.Time = string.Empty;

            step.State = state;
            step.Detail = detail;
            Hint = string.Empty;
        }

        private void Tick()
        {
            Elapsed = FormatTime(DateTime.Now - _startedAt);
            foreach (var step in Steps)
            {
                if (step.IsRunning)
                    step.Time = FormatTime(DateTime.Now - step.StartedAt);
            }
        }

        private static string FormatTime(TimeSpan span)
        {
            return $"{(int)span.TotalMinutes}:{span.Seconds:D2}";
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

        private static (long, DateTime) GetFileStamp(string file)
        {
            var info = new FileInfo(file);
            return info.Exists ? (info.Length, info.LastWriteTimeUtc) : (-1, DateTime.MinValue);
        }

        private static string LogFile => System.IO.Path.Combine(System.IO.Path.GetTempPath(), "MyGitGUI", "Unreal", "last-merge.log");

        private class Revisions
        {
            public string Base { get; init; }
            public string Ours { get; init; }
            public string Theirs { get; init; }
            public string Merged { get; init; }
            public string Report { get; init; }
            public string Result { get; init; }
        }

        private readonly Repository _repo;
        private readonly Conflict _owner;
        private readonly UnrealMergeTarget _target;
        private readonly Mode _mode;
        private readonly string _assetName;
        private readonly UnrealMergeStep _read;
        private readonly UnrealMergeStep _analyze;
        private readonly UnrealMergeStep _decide;
        private readonly UnrealMergeStep _save;
        private DateTime _startedAt;
        private string _elapsed = "0:00";
        private string _hint = string.Empty;
        private Process _process;
        private bool _terminated;
    }
}
