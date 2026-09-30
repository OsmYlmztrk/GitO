using System;

namespace SourceGit.Models
{
    public enum ConflictRiskLevel
    {
        Low,
        Medium,
        High,
    }

    public enum ConflictRiskKind
    {
        /// <summary>A remote branch changed a file you are changing.</summary>
        BranchChange,
        /// <summary>Someone else holds the LFS lock of a file you are changing (catches unpushed work).</summary>
        LockedByOther,
        /// <summary>You are editing a lockable file without holding its lock.</summary>
        UnlockedEdit,
    }

    /// <summary>
    ///     MyGitGUI: one conflict-radar finding.
    /// </summary>
    public class ConflictRisk
    {
        public ConflictRiskKind Kind { get; init; } = ConflictRiskKind.BranchChange;
        public ConflictRiskLevel Level { get; init; } = ConflictRiskLevel.Medium;
        public string Path { get; init; } = string.Empty;
        public string Branch { get; init; } = string.Empty;
        public string SHA { get; init; } = string.Empty;
        public string Author { get; init; } = string.Empty;
        public string AuthorEmail { get; init; } = string.Empty;
        public ulong Time { get; init; } = 0;
        public bool IsUncommitted { get; init; } = false;
        public bool IsLockable { get; init; } = false;
        public bool IsLocked { get; init; } = false;
        public bool LinesOverlap { get; init; } = false;

        public string Key => Kind switch
        {
            ConflictRiskKind.LockedByOther => $"lock|{Path}|{Author}",
            ConflictRiskKind.UnlockedEdit => $"unlocked|{Path}",
            _ => $"{Branch}|{Path}|{SHA}",
        };

        public string FileName => System.IO.Path.GetFileName(Path);
        public string Folder => System.IO.Path.GetDirectoryName(Path)?.Replace('\\', '/') ?? string.Empty;
        public string Extension => System.IO.Path.GetExtension(Path);
        public bool IsHigh => Level == ConflictRiskLevel.High;
        public bool IsMedium => Level == ConflictRiskLevel.Medium;
        public bool IsLow => Level == ConflictRiskLevel.Low;
        public bool IsReminder => Kind == ConflictRiskKind.UnlockedEdit;
        public bool CanGoToCommit => Kind == ConflictRiskKind.BranchChange && !string.IsNullOrEmpty(SHA);
        public bool CanLock => IsLockable && !IsLocked && Kind != ConflictRiskKind.LockedByOther;
        public bool HasBranch => Kind == ConflictRiskKind.BranchChange;
        public bool HasAuthor => Kind != ConflictRiskKind.UnlockedEdit && !string.IsNullOrEmpty(Author);

        /// <summary>Shown as a toast (and counted as an alert). Low risks only appear in the list.</summary>
        public bool IsAlert => Level != ConflictRiskLevel.Low || IsReminder;

        /// <summary>Plays a sound. Reminders are shown quietly.</summary>
        public bool IsLoud => Level != ConflictRiskLevel.Low && !IsReminder;

        public string Title => Kind switch
        {
            ConflictRiskKind.LockedByOther => "Dosya başkasında kilitli",
            ConflictRiskKind.UnlockedEdit => "Kilitlemeden değiştiriyorsun",
            _ => "Çakışma riski",
        };

        public string LevelText => IsReminder ? "Hatırlatma" : Level switch
        {
            ConflictRiskLevel.High => "Yüksek",
            ConflictRiskLevel.Medium => "Orta",
            _ => "Düşük",
        };

        public string WhenText => FormatWhen(Time);

        public string Summary => Kind switch
        {
            ConflictRiskKind.LockedByOther => $"{Author} bu dosyayı LFS ile kilitledi ({WhenText}); büyük ihtimalle üzerinde çalışıyor.",
            ConflictRiskKind.UnlockedEdit => "Bu dosya kilitlenebilir ama sende kilidi yok. Başkası da aynı anda değiştirebilir.",
            _ => $"{Author}, {Branch} branch'inde bu dosyayı değiştirdi ({WhenText}).",
        };

        public string YourSide => IsUncommitted
            ? "Sende bu dosyada commit'lenmemiş değişiklik var."
            : "Senin push edilmemiş commit'lerinde de bu dosya var.";

        public string Advice
        {
            get
            {
                switch (Kind)
                {
                    case ConflictRiskKind.LockedByOther:
                        return "Push etmeden önce konuşun; kilit onda olduğu için push'un reddedilebilir.";
                    case ConflictRiskKind.UnlockedEdit:
                        return "Dosyayı kilitlersen başkası üzerinde çalışamaz ve binary conflict oluşmaz.";
                }

                if (IsHigh && !LinesOverlap)
                    return "Binary dosya: iki taraf birleştirilemez. Ekip arkadaşınla konuş ya da dosyayı kilitle.";
                if (LinesOverlap)
                    return "Aynı satırlar iki tarafta da değişti; merge'de conflict neredeyse kesin.";
                return "Dosyanın farklı bölümleri değişti; git büyük ihtimalle kendisi birleştirir.";
            }
        }

        public static string FormatWhen(ulong unixSeconds)
        {
            if (unixSeconds == 0)
                return "zamanı bilinmiyor";

            var elapsed = DateTime.UtcNow - DateTime.UnixEpoch.AddSeconds(unixSeconds);
            if (elapsed.TotalMinutes < 1)
                return "az önce";
            if (elapsed.TotalHours < 1)
                return $"{(int)elapsed.TotalMinutes} dk önce";
            if (elapsed.TotalDays < 1)
                return $"{(int)elapsed.TotalHours} saat önce";
            return $"{(int)elapsed.TotalDays} gün önce";
        }

        private static readonly string[] s_binaryExtensions =
        [
            ".uasset", ".umap", ".upk", ".udk",
            ".unity", ".prefab", ".asset", ".mat", ".controller", ".anim",
            ".fbx", ".obj", ".blend", ".max", ".ma", ".mb",
            ".png", ".tga", ".psd", ".jpg", ".jpeg", ".exr", ".hdr", ".tif", ".tiff", ".dds",
            ".wav", ".mp3", ".ogg", ".bank", ".mp4", ".mov",
            ".dll", ".exe", ".pdb", ".lib", ".so", ".dylib", ".zip",
        ];

        public static bool LooksBinary(string path)
        {
            var ext = System.IO.Path.GetExtension(path);
            return !string.IsNullOrEmpty(ext) && Array.Exists(s_binaryExtensions, x => x.Equals(ext, StringComparison.OrdinalIgnoreCase));
        }
    }
}
