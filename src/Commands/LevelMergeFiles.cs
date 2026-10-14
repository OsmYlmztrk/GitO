namespace SourceGit.Commands
{
    /// <summary>
    ///     MyGitGUI: writes ours into the working copy of a conflicted file without resolving it
    ///     (git checkout --ours), so Unreal opens the level with ours.
    /// </summary>
    public class CheckoutOursKeepConflict : Command
    {
        public CheckoutOursKeepConflict(string repo, string file)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"checkout --ours -- {file.Quoted()}";
        }
    }

    /// <summary>
    ///     MyGitGUI: removes a conflicted file from the index and the working copy (the level merge deleted the actor).
    /// </summary>
    public class RemoveConflictedFile : Command
    {
        public RemoveConflictedFile(string repo, string file)
        {
            WorkingDirectory = repo;
            Context = repo;
            Args = $"rm --force --ignore-unmatch -- {file.Quoted()}";
        }
    }
}
