using Avalonia.Controls;
using Avalonia.Interactivity;

namespace SourceGit.Views
{
    public partial class ConflictRadarPanel : UserControl
    {
        public ConflictRadarPanel()
        {
            InitializeComponent();
        }

        private void OnRescan(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar)
                radar.Rescan();

            e.Handled = true;
        }

        private void OnGoToCommit(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictRisk risk })
                radar.GoToCommit(risk);

            e.Handled = true;
        }

        private void OnIgnore(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictRisk risk })
                radar.Ignore(risk);

            e.Handled = true;
        }
    }
}
