using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: report of the level merge window in Unreal (-GitLevelMergeUI -Report=...).
    /// </summary>
    public class UnrealLevelMergeReport
    {
        [JsonPropertyName("written")]
        public bool Written { get; set; } = false;

        [JsonPropertyName("actors")]
        public List<UnrealLevelMergeActor> Actors { get; set; } = [];

        [JsonPropertyName("problems")]
        public List<string> Problems { get; set; } = [];
    }

    public class UnrealLevelMergeActor
    {
        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("label")]
        public string Label { get; set; } = string.Empty;

        /// <summary>"saved", "unchanged" (the file in the working copy is kept as is) or "deleted".</summary>
        [JsonPropertyName("result")]
        public string Result { get; set; } = string.Empty;
    }

    /// <summary>
    ///     MyGitGUI: what GitO hands to the level merge window (-Manifest=...).
    /// </summary>
    public class UnrealLevelMergeManifest
    {
        [JsonPropertyName("level")]
        public string Level { get; set; } = string.Empty;

        [JsonPropertyName("oursLabel")]
        public string OursLabel { get; set; } = string.Empty;

        [JsonPropertyName("theirsLabel")]
        public string TheirsLabel { get; set; } = string.Empty;

        [JsonPropertyName("actors")]
        public List<UnrealLevelMergeManifestActor> Actors { get; set; } = [];
    }

    public class UnrealLevelMergeManifestActor
    {
        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("package")]
        public string Package { get; set; } = string.Empty;

        [JsonPropertyName("label")]
        public string Label { get; set; } = string.Empty;

        [JsonPropertyName("className")]
        public string ClassName { get; set; } = string.Empty;

        /// <summary>Temp files of the three versions; empty when that side has no such actor.</summary>
        [JsonPropertyName("base")]
        public string Base { get; set; } = string.Empty;

        [JsonPropertyName("ours")]
        public string Ours { get; set; } = string.Empty;

        [JsonPropertyName("theirs")]
        public string Theirs { get; set; } = string.Empty;
    }
}
