using System;
using System.Reflection;
using System.Text.RegularExpressions;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace SourceGit.Views
{
    public partial class About : ChromelessWindow
    {
        public About()
        {
            CloseOnESC = true;
            InitializeComponent();

            var assembly = Assembly.GetExecutingAssembly();
            var meta = assembly.GetCustomAttributes<AssemblyMetadataAttribute>();
            var foundFriendlyVersion = false;
            foreach (var attr in meta)
            {
                if (attr.Key.Equals("BuildDate", StringComparison.OrdinalIgnoreCase) && DateTime.TryParse(attr.Value, out var date))
                {
                    TxtReleaseDate.Text = App.Text("About.ReleaseDate", Models.DateTimeFormat.Format(date, true));
                }
                else if (attr.Key.Equals("FriendlyVersion", StringComparison.OrdinalIgnoreCase) && REG_FRIENDLY_VERSION().IsMatch(attr.Value))
                {
                    foundFriendlyVersion = true;
                    TxtVersion.Text = attr.Value;
                }
            }

            if (!foundFriendlyVersion)
            {
                var ver = assembly.GetName().Version;
                if (ver != null)
                    TxtVersion.Text = $"v{ver.Major}.{ver.Minor}.{Math.Max(ver.Build, 0)}"; // MyGitGUI: three-part GitO versions
            }

            var copyright = assembly.GetCustomAttribute<AssemblyCopyrightAttribute>();
            if (copyright != null)
                TxtCopyright.Text = copyright.Copyright;
        }

        private void OnVisitReleaseNotes(object _, RoutedEventArgs e)
        {
            var ver = TxtVersion.Text ?? string.Empty;
            var endOfTagIdx = ver.IndexOf('-');
            if (endOfTagIdx > 0)
                ver = ver.Substring(0, endOfTagIdx);

            // MyGitGUI: GitO releases
            Native.OS.OpenBrowser($"https://github.com/OsmYlmztrk/GitO/releases/tag/{ver}");
            e.Handled = true;
        }

        private void OnVisitSourceCode(object _, RoutedEventArgs e)
        {
            // MyGitGUI: GitO repo
            Native.OS.OpenBrowser("https://github.com/OsmYlmztrk/GitO");
            e.Handled = true;
        }

        // MyGitGUI: SourceGit attribution link
        private void OnVisitUpstream(object _, PointerPressedEventArgs e)
        {
            Native.OS.OpenBrowser("https://github.com/sourcegit-scm/sourcegit");
            e.Handled = true;
        }

        // MyGitGUI: GitO tags are vX.Y.Z (SourceGit's year-based tags fall back to the assembly version)
        [GeneratedRegex(@"^v\d{1,3}\.\d+\.\d+(?:\-\d+\-[0-9a-f]{8})?(?:\-dirty)?$")]
        private static partial Regex REG_FRIENDLY_VERSION();
    }
}
