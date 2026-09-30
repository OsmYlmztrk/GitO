using System;
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

        /// <summary>
        ///     "Yoksay" choices for a risk: this file / this branch / this person / all files of this type.
        /// </summary>
        public static ContextMenu CreateIgnoreMenu(ViewModels.ConflictRadar radar, Models.ConflictRisk risk)
        {
            var menu = new ContextMenu();

            void Add(string header, Models.ConflictIgnoreRuleType type)
            {
                var item = new MenuItem() { Header = header };
                item.Click += (_, e) =>
                {
                    radar.Ignore(risk, type);
                    e.Handled = true;
                };
                menu.Items.Add(item);
            }

            Add($"Bu dosyayı yoksay ({risk.FileName})", Models.ConflictIgnoreRuleType.File);
            if (risk.HasBranch)
                Add($"Bu branch'i yoksay ({risk.Branch})", Models.ConflictIgnoreRuleType.Branch);
            if (risk.HasAuthor)
                Add($"Bu kişiyi yoksay ({risk.Author})", Models.ConflictIgnoreRuleType.Author);
            if (!string.IsNullOrEmpty(risk.Extension))
                Add($"Tüm *{risk.Extension} dosyalarını yoksay", Models.ConflictIgnoreRuleType.Pattern);

            return menu;
        }

        public static ContextMenu CreateSnoozeMenu(ViewModels.ConflictRadar radar)
        {
            var menu = new ContextMenu();

            var hour = new MenuItem() { Header = "1 saat sessize al" };
            hour.Click += (_, e) =>
            {
                radar.Snooze(TimeSpan.FromHours(1));
                e.Handled = true;
            };

            var today = new MenuItem() { Header = "Bugün için sessize al" };
            today.Click += (_, e) =>
            {
                radar.SnoozeUntilTonight();
                e.Handled = true;
            };

            menu.Items.Add(hour);
            menu.Items.Add(today);

            if (radar.IsSnoozed)
            {
                var off = new MenuItem() { Header = "Sessizliği kaldır" };
                off.Click += (_, e) =>
                {
                    radar.Unsnooze();
                    e.Handled = true;
                };
                menu.Items.Add(new MenuItem() { Header = "-" });
                menu.Items.Add(off);
            }

            return menu;
        }

        private void OnOpenSnoozeMenu(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control control)
                CreateSnoozeMenu(radar).Open(control);

            e.Handled = true;
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

        private async void OnLock(object sender, RoutedEventArgs e)
        {
            e.Handled = true;
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictRisk risk })
                await radar.LockFileAsync(risk);
        }

        private void OnOpenIgnoreMenu(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictRisk risk } control)
                CreateIgnoreMenu(radar, risk).Open(control);

            e.Handled = true;
        }

        private void OnHistoryGoToCommit(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictNotificationRecord record })
                radar.GoToCommit(record);

            e.Handled = true;
        }

        private void OnHistoryRestore(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictNotificationRecord record })
                radar.Restore(record);

            e.Handled = true;
        }

        private void OnClearHistory(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar)
                radar.ClearHistory();

            e.Handled = true;
        }

        private void OnRemoveIgnoreRule(object sender, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.ConflictRadar radar && sender is Control { DataContext: Models.ConflictIgnoreRule rule })
                radar.RemoveIgnoreRule(rule);

            e.Handled = true;
        }
    }
}
