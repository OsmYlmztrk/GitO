using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using Avalonia.Threading;

namespace SourceGit.ViewModels
{
    /// <summary>
    ///     What a level merge needs: the level and its conflicted actor files.
    /// </summary>
    public class UnrealLevelMergeTarget
    {
        public string LevelName { get; set; }
        public string LevelPackage { get; set; }
        public string Project { get; set; }
        public string Editor { get; set; }
        public string Plugin { get; set; }
        public string OursLabel { get; set; }
        public string TheirsLabel { get; set; }
        public List<Models.Change> Actors { get; set; } = [];
    }

    /// <summary>
    ///     MyGitGUI: merges the conflicted actors of a One File Per Actor level in Unreal, by hand (user, 2026-10-14):
    ///     GitO puts ours into the level, Unreal opens the level with our level merge window, the user picks Ours or
    ///     Theirs for every change there, and GitO stages what Unreal saved. Nothing is merged automatically.
    /// </summary>
    public class UnrealLevelMerge : Popup
    {
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

        public UnrealLevelMerge(Repository repo, UnrealLevelMergeTarget target)
        {
            _repo = repo;
            _target = target;

            Title = $"Merge level {target.LevelName}";
            OursLabel = string.IsNullOrEmpty(target.OursLabel) ? "your branch" : target.OursLabel;
            TheirsLabel = string.IsNullOrEmpty(target.TheirsLabel) ? "incoming" : target.TheirsLabel;
            CanTerminate = true;

            _check = new UnrealMergeStep("Check that Unreal Editor is closed");
            _read = new UnrealMergeStep($"Get Base, Ours and Theirs of {target.Actors.Count} actor(s) from Git");
            _prepare = new UnrealMergeStep("Put Ours into the level");
            _decide = new UnrealMergeStep("Decide every change in Unreal");
            _save = new UnrealMergeStep("Stage the result");
            Steps.AddRange([_check, _read, _prepare, _decide, _save]);
        }

        public override async Task<bool> Sure()
        {
            _startedAt = DateTime.Now;
            var timer = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => Tick());
            timer.Start();

            var tempDir = Path.Combine(Path.GetTempPath(), "MyGitGUI", "UnrealLevel", Guid.NewGuid().ToString("N"));
            string message;
            var outcome = Outcome.Done;
            try
            {
                (message, outcome) = await RunAsync(tempDir);
            }
            catch (Exception e)
            {
                (message, outcome) = ($"Couldn't merge level {_target.LevelName}: {e.Message}", Outcome.Error);
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
                    // A leftover temp folder is harmless.
                }
            }

            if (outcome == Outcome.Warning)
                _repo.SendWarning(message);
            else
                _repo.SendNotification(message, outcome == Outcome.Error);
            App.RequestAttention();
            _repo.MarkWorkingCopyDirtyManually();
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

        private async Task<(string, Outcome)> RunAsync(string tempDir)
        {
            // 0. An open editor would hold the old actors and could save them over the merge result.
            Begin(_check, "Checking for a running Unreal Editor...");
            if (IsUnrealRunning())
            {
                End(_check, UnrealMergeStepState.Failed, "Unreal Editor is running");
                return ($"Level {_target.LevelName} wasn't merged: Unreal Editor is running. Save your work, close Unreal, then try again.", Outcome.Warning);
            }
            End(_check, UnrealMergeStepState.Done, string.Empty);

            // 1. Base / Ours / Theirs of every actor (LFS is resolved by git's filters).
            Begin(_read, "Reading the conflicting versions from Git...");
            Directory.CreateDirectory(tempDir);
            var manifest = new Models.UnrealLevelMergeManifest()
            {
                Level = _target.LevelPackage,
                OursLabel = OursLabel,
                TheirsLabel = TheirsLabel,
            };

            for (var i = 0; i < _target.Actors.Count; i++)
            {
                var change = _target.Actors[i];
                var stages = await new Commands.CheckoutConflictStages(_repo.FullPath, change.Path).GetResultAsync();
                manifest.Actors.Add(new Models.UnrealLevelMergeManifestActor()
                {
                    Path = change.Path,
                    Package = Models.UnrealEngine.GetPackageName(_target.Project, Path.Combine(_repo.FullPath, change.Path)),
                    Label = change.UnrealActor?.Label ?? Path.GetFileNameWithoutExtension(change.Path),
                    ClassName = change.UnrealActor?.ClassName ?? string.Empty,
                    Base = MoveTo(stages.Base, Path.Combine(tempDir, $"{i}_Base.uasset")),
                    Ours = MoveTo(stages.Ours, Path.Combine(tempDir, $"{i}_Ours.uasset")),
                    Theirs = MoveTo(stages.Theirs, Path.Combine(tempDir, $"{i}_Theirs.uasset")),
                });
            }
            End(_read, UnrealMergeStepState.Done, string.Empty);

            // 2. The working copy holds a conflicted LFS pointer where both sides changed an actor; Unreal needs ours.
            Begin(_prepare, "Writing Ours into the working copy (the files stay conflicted)...");
            foreach (var actor in manifest.Actors)
            {
                if (!string.IsNullOrEmpty(actor.Ours) && !string.IsNullOrEmpty(actor.Theirs))
                    await new Commands.CheckoutOursKeepConflict(_repo.FullPath, actor.Path).ExecAsync();
            }

            var manifestFile = Path.Combine(tempDir, "manifest.json");
            var reportFile = Path.Combine(tempDir, "report.json");
            await File.WriteAllTextAsync(manifestFile, JsonSerializer.Serialize(manifest, JsonCodeGen.Default.UnrealLevelMergeManifest));
            End(_prepare, UnrealMergeStepState.Done, string.Empty);

            // 3. The user decides everything in Unreal.
            Begin(_decide, $"Switch to Unreal: the level opens with the \"Merge level {_target.LevelName}\" window. " +
                "Pick Ours or Theirs for every change, then press Merge. Cancel or closing the window changes nothing.");
            var args = $"{_target.Project.Quoted()} {_target.LevelPackage} -GitLevelMergeUI -Manifest={manifestFile.Quoted()} -Report={reportFile.Quoted()} " +
                $"-PLUGIN={_target.Plugin.Quoted()} -EnablePlugins=GitBlueprintMerge -nopause -abslog={LogFile.Quoted()}";
            Directory.CreateDirectory(Path.GetDirectoryName(LogFile)!);
            _process = Process.Start(new ProcessStartInfo(_target.Editor, args) { UseShellExecute = false });
            if (_process == null)
            {
                End(_decide, UnrealMergeStepState.Failed, "Could not start Unreal");
                return ("Could not start Unreal Editor.", Outcome.Error);
            }

            using (_process)
            {
                await _process.WaitForExitAsync();
            }
            _process = null;

            if (_terminated || !File.Exists(reportFile))
            {
                End(_decide, UnrealMergeStepState.Skipped, _terminated ? "Cancelled" : "Unreal closed");
                return ($"Level {_target.LevelName} wasn't merged; its actors are still conflicted.", Outcome.Warning);
            }

            Models.UnrealLevelMergeReport report;
            await using (var stream = File.OpenRead(reportFile))
                report = await JsonSerializer.DeserializeAsync(stream, JsonCodeGen.Default.UnrealLevelMergeReport);

            if (report is not { Written: true })
            {
                var problems = report is { Problems.Count: > 0 } ? $" {string.Join(" ", report.Problems)}" : string.Empty;
                End(_decide, UnrealMergeStepState.Skipped, "Not merged");
                return ($"Level {_target.LevelName} wasn't merged; its actors are still conflicted.{problems}", report is { Problems.Count: > 0 } ? Outcome.Error : Outcome.Warning);
            }
            End(_decide, UnrealMergeStepState.Done, string.Empty);

            // 4. Stage what Unreal saved; deleted actors lose their file.
            Begin(_save, "Staging...");
            var log = _repo.CreateLog("Merge level");
            var toAdd = new List<string>();
            var deleted = 0;
            foreach (var actor in report.Actors)
            {
                if (actor.Result == "deleted")
                {
                    await new Commands.RemoveConflictedFile(_repo.FullPath, actor.Path).Use(log).ExecAsync();
                    deleted++;
                }
                else
                {
                    toAdd.Add(actor.Path);
                }
            }

            if (toAdd.Count > 0)
            {
                var pathSpecFile = Path.GetTempFileName();
                await File.WriteAllLinesAsync(pathSpecFile, toAdd);
                await new Commands.Add(_repo.FullPath, pathSpecFile).Use(log).ExecAsync();
                File.Delete(pathSpecFile);
            }
            log.Complete();
            End(_save, UnrealMergeStepState.Done, string.Empty);

            var result = $"Level {_target.LevelName} merged and staged: {toAdd.Count} actor(s) kept";
            if (deleted > 0)
                result += $", {deleted} deleted";
            return (result + ".", Outcome.Done);
        }

        private static bool IsUnrealRunning()
        {
            var processes = Process.GetProcessesByName("UnrealEditor");
            foreach (var p in processes)
                p.Dispose();
            return processes.Length > 0;
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
            step.Time = step.IsRunning ? FormatTime(DateTime.Now - step.StartedAt) : string.Empty;
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
                return string.Empty;

            File.Move(src, dst, true);
            return dst;
        }

        private enum Outcome
        {
            Done,
            Warning,
            Error,
        }

        private static string LogFile => Path.Combine(Path.GetTempPath(), "MyGitGUI", "Unreal", "last-level-merge.log");

        private readonly Repository _repo;
        private readonly UnrealLevelMergeTarget _target;
        private readonly UnrealMergeStep _check;
        private readonly UnrealMergeStep _read;
        private readonly UnrealMergeStep _prepare;
        private readonly UnrealMergeStep _decide;
        private readonly UnrealMergeStep _save;
        private DateTime _startedAt;
        private string _elapsed = "0:00";
        private string _hint = string.Empty;
        private Process _process;
        private bool _terminated;
    }
}
