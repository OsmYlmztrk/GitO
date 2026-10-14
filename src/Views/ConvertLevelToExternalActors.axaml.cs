using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.VisualTree;

namespace SourceGit.Views
{
    public partial class ConvertLevelToExternalActors : UserControl
    {
        public ConvertLevelToExternalActors()
        {
            InitializeComponent();
        }

        private void OnDoItMyself(object _, RoutedEventArgs e)
        {
            Native.OS.OpenBrowser(Models.UnrealLevel.GuideUrl);
            if (this.FindAncestorOfType<LauncherPage>() is { DataContext: ViewModels.LauncherPage page })
                page.CancelPopup();

            e.Handled = true;
        }
    }
}
