using System.Collections.Generic;
using System.Text.Json.Serialization;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: report written by the GitBlueprintMerge Unreal commandlet (-Report=...).
    /// </summary>
    public class BlueprintMergeReport
    {
        [JsonPropertyName("asset")]
        public string Asset { get; set; } = string.Empty;

        [JsonPropertyName("canAutoMerge")]
        public bool CanAutoMerge { get; set; } = false;

        [JsonPropertyName("written")]
        public bool Written { get; set; } = false;

        /// <summary>The person edited the result by hand in the merge window and confirmed those edits.</summary>
        [JsonPropertyName("editedByHand")]
        public bool EditedByHand { get; set; } = false;

        [JsonPropertyName("summary")]
        public BlueprintMergeSummary Summary { get; set; } = new();

        /// <summary>One entry per conflict group: the decisions a person has to make.</summary>
        [JsonPropertyName("conflicts")]
        public List<BlueprintMergeConflict> Conflicts { get; set; } = [];

        [JsonPropertyName("changes")]
        public List<BlueprintMergeChange> Changes { get; set; } = [];

        /// <summary>Changes from theirs the plugin can't merge yet (graphs, components, ...), in words.</summary>
        [JsonPropertyName("unsupported")]
        public List<string> Unsupported { get; set; } = [];

        /// <summary>What the merge left out because the person chose to redo it by hand.</summary>
        [JsonPropertyName("leftOut")]
        public List<string> LeftOut { get; set; } = [];

        [JsonPropertyName("problems")]
        public List<string> Problems { get; set; } = [];
    }

    public class BlueprintMergeSummary
    {
        [JsonPropertyName("ours")]
        public int Ours { get; set; } = 0;

        [JsonPropertyName("theirs")]
        public int Theirs { get; set; } = 0;

        [JsonPropertyName("both")]
        public int Both { get; set; } = 0;

        [JsonPropertyName("conflicts")]
        public int Conflicts { get; set; } = 0;

        /// <summary>Conflict groups decided in the merge window.</summary>
        [JsonPropertyName("resolved")]
        public int Resolved { get; set; } = 0;
    }

    public class BlueprintMergeConflict
    {
        [JsonPropertyName("group")]
        public int Group { get; set; } = 0;

        [JsonPropertyName("text")]
        public string Text { get; set; } = string.Empty;

        [JsonPropertyName("details")]
        public List<string> Details { get; set; } = [];
    }

    public class BlueprintMergeChange
    {
        /// <summary>"ours", "theirs", "both" or "conflict".</summary>
        [JsonPropertyName("resolution")]
        public string Resolution { get; set; } = string.Empty;

        [JsonPropertyName("text")]
        public string Text { get; set; } = string.Empty;

        [JsonPropertyName("reason")]
        public string Reason { get; set; } = string.Empty;

        public bool IsConflict => Resolution == "conflict";
        public bool IsOurs => Resolution == "ours";
        public bool IsTheirs => Resolution == "theirs";
        public string Tag => Resolution.ToUpperInvariant();
    }
}
