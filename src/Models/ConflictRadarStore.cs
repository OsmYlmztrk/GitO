using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace SourceGit.Models
{
    public enum ConflictIgnoreRuleType
    {
        File,
        Branch,
        Author,
        Pattern,
    }

    /// <summary>
    ///     MyGitGUI: a persistent "stop warning me about this" rule of the conflict radar.
    /// </summary>
    public class ConflictIgnoreRule
    {
        public ConflictIgnoreRuleType Type { get; set; } = ConflictIgnoreRuleType.File;
        public string Value { get; set; } = string.Empty;
        public DateTime CreatedAt { get; set; } = DateTime.Now;

        public string Description => Type switch
        {
            ConflictIgnoreRuleType.Branch => $"Branch: {Value}",
            ConflictIgnoreRuleType.Author => $"Kişi: {Value}",
            ConflictIgnoreRuleType.Pattern => $"Dosya kalıbı: {Value}",
            _ => $"Dosya: {Value}",
        };

        public bool Matches(ConflictRisk risk)
        {
            switch (Type)
            {
                case ConflictIgnoreRuleType.File:
                    return risk.Path.Equals(Value, StringComparison.Ordinal);
                case ConflictIgnoreRuleType.Branch:
                    return risk.Kind == ConflictRiskKind.BranchChange && risk.Branch.Equals(Value, StringComparison.Ordinal);
                case ConflictIgnoreRuleType.Author:
                    return risk.Kind != ConflictRiskKind.UnlockedEdit &&
                           (risk.Author.Equals(Value, StringComparison.OrdinalIgnoreCase) ||
                            risk.AuthorEmail.Equals(Value, StringComparison.OrdinalIgnoreCase));
                case ConflictIgnoreRuleType.Pattern:
                    return GlobMatches(Value, risk.Path) || GlobMatches(Value, risk.FileName);
            }

            return false;
        }

        private static bool GlobMatches(string glob, string text)
        {
            var regex = "^" + Regex.Escape(glob).Replace(@"\*", ".*").Replace(@"\?", ".") + "$";
            return Regex.IsMatch(text, regex, RegexOptions.IgnoreCase);
        }
    }

    public enum ConflictNotificationStatus
    {
        /// <summary>Still a risk, not handled yet.</summary>
        Active,
        /// <summary>Toast closed with ×.</summary>
        Dismissed,
        /// <summary>Hidden by an ignore rule.</summary>
        Ignored,
        /// <summary>Arrived while the radar was snoozed (no toast, no sound).</summary>
        Snoozed,
        /// <summary>The risk went away (pushed, merged, change reverted...).</summary>
        Resolved,
    }

    /// <summary>
    ///     MyGitGUI: an entry in the notification history ("bildirim deposu").
    /// </summary>
    public class ConflictNotificationRecord
    {
        public string Key { get; set; } = string.Empty;
        public ConflictRiskKind Kind { get; set; } = ConflictRiskKind.BranchChange;
        public ConflictRiskLevel Level { get; set; } = ConflictRiskLevel.Medium;
        public string Path { get; set; } = string.Empty;
        public string Branch { get; set; } = string.Empty;
        public string SHA { get; set; } = string.Empty;
        public string Author { get; set; } = string.Empty;
        public string AuthorEmail { get; set; } = string.Empty;
        public ulong Time { get; set; } = 0;
        public bool IsUncommitted { get; set; } = false;
        public bool LinesOverlap { get; set; } = false;
        public DateTime ReceivedAt { get; set; } = DateTime.Now;
        public ConflictNotificationStatus Status { get; set; } = ConflictNotificationStatus.Active;
        public DateTime StatusAt { get; set; } = DateTime.Now;

        // Display helpers (read-only, not serialized).
        public string FileName => System.IO.Path.GetFileName(Path);
        public string Folder => System.IO.Path.GetDirectoryName(Path)?.Replace('\\', '/') ?? string.Empty;
        public bool IsHigh => Level == ConflictRiskLevel.High;
        public bool IsLow => Level == ConflictRiskLevel.Low;
        public bool CanGoToCommit => Kind == ConflictRiskKind.BranchChange && !string.IsNullOrEmpty(SHA);
        public string Title => ToRisk().Title;
        public string Summary => ToRisk().Summary;
        public string LevelText => ToRisk().LevelText;
        public string ReceivedText => ReceivedAt.ToString("dd.MM.yyyy HH:mm");
        public bool CanRestore => Status is ConflictNotificationStatus.Dismissed or ConflictNotificationStatus.Ignored or ConflictNotificationStatus.Snoozed;
        public bool IsActive => Status == ConflictNotificationStatus.Active;
        public bool IsClosed => Status is ConflictNotificationStatus.Dismissed or ConflictNotificationStatus.Ignored;

        public string StatusText => Status switch
        {
            ConflictNotificationStatus.Dismissed => "Kapatıldı",
            ConflictNotificationStatus.Ignored => "Yoksayıldı",
            ConflictNotificationStatus.Snoozed => "Sessizde geldi",
            ConflictNotificationStatus.Resolved => "Çözüldü",
            _ => "Aktif",
        };

        public static ConflictNotificationRecord From(ConflictRisk risk) => new()
        {
            Key = risk.Key,
            Kind = risk.Kind,
            Level = risk.Level,
            Path = risk.Path,
            Branch = risk.Branch,
            SHA = risk.SHA,
            Author = risk.Author,
            AuthorEmail = risk.AuthorEmail,
            Time = risk.Time,
            IsUncommitted = risk.IsUncommitted,
            LinesOverlap = risk.LinesOverlap,
        };

        public ConflictRisk ToRisk() => new()
        {
            Kind = Kind,
            Level = Level,
            Path = Path,
            Branch = Branch,
            SHA = SHA,
            Author = Author,
            AuthorEmail = AuthorEmail,
            Time = Time,
            IsUncommitted = IsUncommitted,
            LinesOverlap = LinesOverlap,
        };
    }

    /// <summary>
    ///     MyGitGUI: per-repository persistent state of the conflict radar, stored in
    ///     `.git/mygitgui/conflict-radar.json` (never committed).
    /// </summary>
    public class ConflictRadarStore
    {
        public List<ConflictIgnoreRule> IgnoreRules { get; set; } = [];
        public DateTime SnoozeUntil { get; set; } = DateTime.MinValue;
        public List<ConflictNotificationRecord> History { get; set; } = [];

        public const int MaxHistory = 500;

        public static ConflictRadarStore Load(string gitDir)
        {
            try
            {
                var file = GetFile(gitDir);
                if (File.Exists(file))
                {
                    using var stream = File.OpenRead(file);
                    return JsonSerializer.Deserialize(stream, JsonCodeGen.Default.ConflictRadarStore) ?? new ConflictRadarStore();
                }
            }
            catch
            {
                // A broken file must never break the repository view.
            }

            return new ConflictRadarStore();
        }

        public void Save(string gitDir)
        {
            try
            {
                if (History.Count > MaxHistory)
                    History.RemoveRange(0, History.Count - MaxHistory);

                var file = GetFile(gitDir);
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(file)!);
                var tmp = file + ".tmp";
                File.WriteAllText(tmp, JsonSerializer.Serialize(this, JsonCodeGen.Default.ConflictRadarStore));
                File.Move(tmp, file, true);
            }
            catch
            {
                // Best effort.
            }
        }

        private static string GetFile(string gitDir) => System.IO.Path.Combine(gitDir, "mygitgui", "conflict-radar.json");
    }
}
