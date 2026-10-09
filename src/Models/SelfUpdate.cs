using System;
using System.Reflection;
using System.Text.Json.Serialization;

namespace SourceGit.Models
{
    public class Version
    {
        [JsonPropertyName("name")]
        public string Name { get; set; }

        [JsonPropertyName("tag_name")]
        public string TagName { get; set; }

        [JsonPropertyName("published_at")]
        public DateTime PublishedAt { get; set; }

        [JsonPropertyName("body")]
        public string Body { get; set; }

        [JsonIgnore]
        public System.Version CurrentVersion { get; }

        // MyGitGUI: GitO uses three-part versions (0.0.1)
        [JsonIgnore]
        public string CurrentVersionStr => $"v{CurrentVersion.Major}.{CurrentVersion.Minor}.{Math.Max(CurrentVersion.Build, 0)}";

        [JsonIgnore]
        public bool IsNewVersion => CurrentVersion.CompareTo(new System.Version(TagName.Substring(1))) < 0;

        [JsonIgnore]
        public string ReleaseDateStr => DateTimeFormat.Format(PublishedAt, true);

        // MyGitGUI: Velopack doesn't report a release date
        [JsonIgnore]
        public bool HasReleaseDate => PublishedAt != default;

        /// <summary>MyGitGUI: the Velopack update to download and apply.</summary>
        [JsonIgnore]
        public Velopack.UpdateInfo Update { get; set; }

        public Version()
        {
            var assembly = Assembly.GetExecutingAssembly().GetName();
            CurrentVersion = assembly.Version ?? new System.Version();
        }
    }

    public class AlreadyUpToDate;

    public class SelfUpdateFailed
    {
        public string Reason
        {
            get;
            private set;
        }

        public SelfUpdateFailed(Exception e)
        {
            if (e.InnerException is { } inner)
                Reason = inner.Message;
            else
                Reason = e.Message;
        }
    }
}
