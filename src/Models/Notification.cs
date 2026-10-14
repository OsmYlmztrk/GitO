using System;

namespace SourceGit.Models
{
    public class Notification
    {
        public static event Action<Notification> Raised;

        public string Group { get; set; }
        public string Message { get; set; }
        public bool IsError { get; set; }

        // MyGitGUI: something didn't happen, but nothing failed (orange instead of the green notice).
        public bool IsWarning { get; set; }
        public bool IsNotice => !IsError && !IsWarning;

        public static void Send(string group, string message, bool isError = false, bool isWarning = false)
        {
            Raised?.Invoke(new Notification
            {
                Group = group,
                Message = message,
                IsError = isError,
                IsWarning = isWarning && !isError
            });
        }
    }
}
