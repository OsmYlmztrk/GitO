using System.Text.Json.Serialization;

namespace SourceGit.Models
{
    public class LFSLockOwner
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;
    }

    public class LFSLock
    {
        [JsonPropertyName("id")]
        public string ID { get; set; } = string.Empty;

        [JsonPropertyName("path")]
        public string Path { get; set; } = string.Empty;

        [JsonPropertyName("owner")]
        public LFSLockOwner Owner { get; set; } = null;

        [JsonPropertyName("locked_at")]
        public string LockedAt { get; set; } = string.Empty;
    }

    // MyGitGUI: output of `git lfs locks --verify --json` (locks split into mine / others').
    public class LFSVerifiedLocks
    {
        [JsonPropertyName("ours")]
        public System.Collections.Generic.List<LFSLock> Ours { get; set; } = [];

        [JsonPropertyName("theirs")]
        public System.Collections.Generic.List<LFSLock> Theirs { get; set; } = [];
    }
}
