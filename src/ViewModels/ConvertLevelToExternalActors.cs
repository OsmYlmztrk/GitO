using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Collections;
using Avalonia.Threading;

namespace SourceGit.ViewModels
{
    /// <summary>
    ///     MyGitGUI: turns on One File Per Actor for a level with Unreal's own ConvertLevelsToExternalActors commandlet,
    ///     in the background. Shows the warnings first; the steps replace them once the user presses OK.
    /// </summary>
    public class ConvertLevelToExternalActors : Popup
    {
        public string LevelName { get; }
        public string LevelPath { get; }
        public string ActorsFolder { get; }
        public bool CanLock { get; }
        public AvaloniaList<UnrealMergeStep> Steps { get; } = [];

        public bool LockFirst
        {
            get => _lockFirst;
            set => SetProperty(ref _lockFirst, value);
        }

        public bool HasStarted
        {
            get => _hasStarted;
            private set => SetProperty(ref _hasStarted, value);
        }

        public string Hint
        {
            get => _hint;
            private set => SetProperty(ref _hint, value);
        }

        public ConvertLevelToExternalActors(Repository repo, string levelPath, string project, string editor, string lockRemote)
        {
            _repo = repo;
            _project = project;
            _editor = editor;
            _lockRemote = lockRemote;

            LevelPath = levelPath;
            LevelName = Path.GetFileName(levelPath);
            ActorsFolder = Models.UnrealLevel.GetExternalActorsFolder(levelPath);
            CanLock = !string.IsNullOrEmpty(lockRemote);
            CanTerminate = true;

            _check = new UnrealMergeStep("Check that Unreal Editor is closed");
            _lock = new UnrealMergeStep("Lock the level with Git LFS");
            _convert = new UnrealMergeStep("Convert the level (Unreal, in the background)");
            _refresh = new UnrealMergeStep("Show the new actor files in Local Changes");
        }

        public override async Task<bool> Sure()
        {
            Steps.Clear();
            foreach (var step in new[] { _check, _lock, _convert, _refresh })
            {
                if (step == _lock && !(CanLock && _lockFirst))
                    continue;

                step.State = UnrealMergeStepState.Pending;
                step.Detail = string.Empty;
                step.Time = string.Empty;
                Steps.Add(step);
            }

            HasStarted = true;
            _terminated = false;

            var timer = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => Tick());
            timer.Start();
            try
            {
                return await RunAsync();
            }
            catch (Exception e)
            {
                Fail(FindRunningStep(), e.Message, e.Message);
                return false;
            }
            finally
            {
                timer.Stop();
            }
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

        private async Task<bool> RunAsync()
        {
            // 1. An editor that still has the level open would save the old layout over the converted one.
            Begin(_check, "Checking for a running Unreal Editor...");
            if (IsUnrealRunning())
            {
                Fail(_check, "Unreal Editor is running",
                    "Unreal Editor is running. Save your work in Unreal, close it, then press OK again.");
                return false;
            }
            End(_check, UnrealMergeStepState.Done, string.Empty);

            // 2. Others see that the level is being changed.
            if (Steps.Contains(_lock))
            {
                Begin(_lock, "Locking the level on the server...");
                var log = _repo.CreateLog("LFS Lock");
                var locked = await new Commands.LFS(_repo.FullPath).Use(log).LockAsync(_lockRemote, LevelPath);
                log.Complete();
                if (!locked)
                {
                    Fail(_lock, "Couldn't lock the level",
                        "Couldn't lock the level (maybe someone else holds the lock). Untick \"Lock the level first\" to convert without a lock, or ask them to unlock it.");
                    return false;
                }
                End(_lock, UnrealMergeStepState.Done, "Locked by you. Unlock it after you push.");
            }

            // 3. Unreal's own commandlet: loads the level, moves every actor into its own file, saves.
            Begin(_convert, "Unreal is loading your project in the background (no window). This can take a minute.");
            var errors = await RunCommandletAsync();
            if (_terminated)
            {
                End(_convert, UnrealMergeStepState.Skipped, "Cancelled");
                Hint = "Cancelled. If Unreal had already started saving, check the level in Unreal before you commit.";
                return false;
            }

            if (errors != null)
            {
                Fail(_convert, errors, $"{LevelName} wasn't converted: {errors}");
                return false;
            }

            var count = CountActorFiles();
            End(_convert, UnrealMergeStepState.Done, $"{count} actor file(s) written");

            // 4. The note in Local Changes goes away by itself once the folder exists.
            Begin(_refresh, string.Empty);
            _repo.MarkWorkingCopyDirtyManually();
            End(_refresh, UnrealMergeStepState.Done, string.Empty);

            _repo.SendNotification(
                $"{LevelName} now keeps each actor in its own file ({count} actor files).\n" +
                $"- Commit {LevelName} together with the new {ActorsFolder} folder, then push.\n" +
                "- Ask your team to pull before they edit this level again.\n" +
                "- Open the level in Unreal once to check it.",
                false);
            App.RequestAttention();
            return true;
        }

        /// <summary>
        ///     Null on success, else a readable reason. Unreal's exit code also counts unrelated project errors,
        ///     so the commandlet's own log lines decide.
        /// </summary>
        private async Task<string> RunCommandletAsync()
        {
            var exe = Path.Combine(Path.GetDirectoryName(_editor)!, "UnrealEditor-Cmd.exe");
            if (!File.Exists(exe))
                return "UnrealEditor-Cmd.exe was not found next to Unreal Editor.";

            var packageName = Models.UnrealEngine.GetPackageName(_project, Path.Combine(_repo.FullPath, LevelPath));
            if (string.IsNullOrEmpty(packageName))
                return "The level isn't inside a Content folder.";

            Directory.CreateDirectory(Path.GetDirectoryName(LogFile)!);
            if (File.Exists(LogFile))
                File.Delete(LogFile);

            var args = $"{_project.Quoted()} -run=ConvertLevelsToExternalActors {packageName} -nosourcecontrol -unattended -nullrhi -nosplash -nopause -abslog={LogFile.Quoted()}";
            _process = Process.Start(new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true });
            if (_process == null)
                return "Couldn't start Unreal.";

            using (_process)
            {
                await _process.WaitForExitAsync();
            }
            _process = null;

            if (_terminated)
                return null;

            if (!File.Exists(LogFile))
                return "Unreal stopped before it wrote a log.";

            var done = false;
            var reasons = new List<string>();
            foreach (var line in await File.ReadAllLinesAsync(LogFile))
            {
                var at = line.IndexOf(LogCategory, StringComparison.Ordinal);
                if (at < 0)
                    continue;

                if (line.Contains("Conversion took", StringComparison.Ordinal))
                    done = true;
                else if (line.Contains(": Error:", StringComparison.Ordinal))
                    reasons.Add(Explain(line.Substring(at + LogCategory.Length).Replace(": Error:", string.Empty).Trim()));
            }

            if (done && reasons.Count == 0)
                return null;

            if (reasons.Count == 0)
                reasons.Add($"Unreal stopped before the conversion finished. Details: {LogFile}");
            return string.Join(" ", reasons);
        }

        private static string Explain(string error)
        {
            if (error.Contains("non-stable actor GUIDs", StringComparison.Ordinal))
                return "The level was saved by an old Unreal version. Open it in Unreal, save it once, close Unreal and try again.";
            if (error.Contains("Error saving", StringComparison.Ordinal))
                return $"Unreal couldn't save a file ({error.Replace("Error saving", string.Empty).Trim()}). Is it open in another program?";
            if (error.Contains("Unknown level", StringComparison.Ordinal))
                return "Unreal couldn't find the level in this project.";
            return error;
        }

        private int CountActorFiles()
        {
            var folder = Path.Combine(_repo.FullPath, ActorsFolder);
            return Directory.Exists(folder) ? Directory.GetFiles(folder, "*.uasset", SearchOption.AllDirectories).Length : 0;
        }

        private UnrealMergeStep FindRunningStep()
        {
            foreach (var step in Steps)
            {
                if (step.IsRunning)
                    return step;
            }

            return null;
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

        private void Fail(UnrealMergeStep step, string detail, string hint)
        {
            if (step != null)
                End(step, UnrealMergeStepState.Failed, detail);
            Hint = hint;
        }

        private void Tick()
        {
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

        private const string LogCategory = "LogConvertLevelsToExternalActorsCommandlet";
        private static string LogFile => Path.Combine(Path.GetTempPath(), "MyGitGUI", "Unreal", "last-convert-level.log");

        private readonly Repository _repo;
        private readonly string _project;
        private readonly string _editor;
        private readonly string _lockRemote;
        private readonly UnrealMergeStep _check;
        private readonly UnrealMergeStep _lock;
        private readonly UnrealMergeStep _convert;
        private readonly UnrealMergeStep _refresh;
        private bool _lockFirst = true;
        private bool _hasStarted = false;
        private string _hint = string.Empty;
        private Process _process;
        private bool _terminated;
    }
}
