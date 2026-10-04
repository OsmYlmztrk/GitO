using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.VisualTree;

namespace SourceGit.Views
{
    public partial class Conflict : UserControl
    {
        public Conflict()
        {
            InitializeComponent();
        }

        private void OnPressedSHA(object sender, PointerPressedEventArgs e)
        {
            var repoView = this.FindAncestorOfType<Repository>();
            if (repoView is { DataContext: ViewModels.Repository repo } && sender is TextBlock text)
                repo.NavigateToCommit(text.Text);

            e.Handled = true;
        }

        private async void OnUseTheirs(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.UseTheirsAsync();

            e.Handled = true;
        }

        private async void OnUseMine(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.UseMineAsync();

            e.Handled = true;
        }

        private void OnMerge(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
            {
                var request = vm.CreateOpenMergeEditorRequest();
                this.ShowWindow(request);
            }

            e.Handled = true;
        }

        private async void OnMergeExternal(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.MergeExternalAsync();

            e.Handled = true;
        }

        private async void OnCompareInUnreal(object _, RoutedEventArgs e)
        {
            e.Handled = true;

            if (DataContext is ViewModels.Conflict vm)
                await vm.CompareInUnrealAsync();
        }

        private async void OnMergeInUnreal(object _, RoutedEventArgs e)
        {
            e.Handled = true;

            if (DataContext is ViewModels.Conflict vm)
                await vm.MergeInUnrealAsync();
        }

        private async void OnResolveInUnreal(object _, RoutedEventArgs e)
        {
            e.Handled = true;

            if (DataContext is ViewModels.Conflict vm)
                await vm.ResolveInUnrealAsync();
        }

        private async void OnOpenUnrealMergeTool(object _, RoutedEventArgs e)
        {
            e.Handled = true;

            if (DataContext is ViewModels.Conflict vm)
                await vm.OpenUnrealMergeToolAsync();
        }

        private async void OnLock(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.LockAsync();

            e.Handled = true;
        }

        private async void OnUnlock(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.UnlockAsync(false);

            e.Handled = true;
        }

        private async void OnForceUnlock(object _, RoutedEventArgs e)
        {
            e.Handled = true;

            if (DataContext is not ViewModels.Conflict { Lock: { } lfsLock } vm)
                return;

            var msg = $"Force unlock '{vm.FileName}'? The lock is held by {lfsLock.Owner?.Name}.";
            if (await App.AskConfirmAsync(msg))
                await vm.UnlockAsync(true);
        }

        private async void OnRefreshLock(object _, RoutedEventArgs e)
        {
            if (DataContext is ViewModels.Conflict vm)
                await vm.RefreshLockAsync();

            e.Handled = true;
        }
    }
}
