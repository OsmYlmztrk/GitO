using System;

namespace SourceGit.Models
{
    public enum ConflictRiskLevel
    {
        Medium,
        High,
    }

    /// <summary>
    ///     MyGitGUI: a file you are changing that someone else also changed on a recently active remote branch.
    /// </summary>
    public class ConflictRisk
    {
        public string Path { get; init; } = string.Empty;
        public string Branch { get; init; } = string.Empty;
        public string SHA { get; init; } = string.Empty;
        public string Author { get; init; } = string.Empty;
        public string AuthorEmail { get; init; } = string.Empty;
        public ulong Time { get; init; } = 0;
        public ConflictRiskLevel Level { get; init; } = ConflictRiskLevel.Medium;
        public bool IsUncommitted { get; init; } = false;

        public string Key => $"{Branch}|{Path}|{SHA}";
        public string FileName => System.IO.Path.GetFileName(Path);
        public string Folder => System.IO.Path.GetDirectoryName(Path)?.Replace('\\', '/') ?? string.Empty;
        public bool IsHigh => Level == ConflictRiskLevel.High;
        public string LevelText => IsHigh ? "Yüksek" : "Orta";

        public string WhenText
        {
            get
            {
                var elapsed = DateTime.UtcNow - DateTime.UnixEpoch.AddSeconds(Time);
                if (elapsed.TotalMinutes < 1)
                    return "az önce";
                if (elapsed.TotalHours < 1)
                    return $"{(int)elapsed.TotalMinutes} dk önce";
                if (elapsed.TotalDays < 1)
                    return $"{(int)elapsed.TotalHours} saat önce";
                return $"{(int)elapsed.TotalDays} gün önce";
            }
        }

        public string Summary => $"{Author}, {Branch} branch'inde bu dosyayı değiştirdi ({WhenText}).";

        public string YourSide => IsUncommitted
            ? "Sende bu dosyada commit'lenmemiş değişiklik var."
            : "Senin push edilmemiş commit'lerinde de bu dosya var.";

        public string Advice => IsHigh
            ? "Binary dosya: iki taraf birleştirilemez. Ekip arkadaşınla konuş ya da dosyayı kilitle."
            : "Aynı dosya değişti; merge sırasında conflict çıkabilir.";

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
