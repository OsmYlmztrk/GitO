using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Interactivity;
using Avalonia.Threading;
using AvaloniaEdit;
using AvaloniaEdit.Document;
using AvaloniaEdit.Editing;
using AvaloniaEdit.TextMate;

namespace SourceGit.Views
{
    public class UpdateInfoView : TextEditor
    {
        protected override Type StyleKeyOverride => typeof(TextEditor);

        public UpdateInfoView() : base(new TextArea(), new TextDocument())
        {
            IsReadOnly = true;
            ShowLineNumbers = false;
            WordWrap = true;
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled;
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto;

            TextArea.TextView.Margin = new Thickness(4, 0);
            TextArea.TextView.Options.EnableHyperlinks = false;
            TextArea.TextView.Options.EnableEmailHyperlinks = false;
        }

        protected override void OnLoaded(RoutedEventArgs e)
        {
            base.OnLoaded(e);

            if (_textMate == null)
            {
                _textMate = Models.TextMateHelper.CreateForEditor(this);
                Models.TextMateHelper.SetGrammarByFileName(_textMate, "README.md");
            }
        }

        protected override void OnUnloaded(RoutedEventArgs e)
        {
            base.OnUnloaded(e);

            if (_textMate != null)
            {
                _textMate.Dispose();
                _textMate = null;
            }

            GC.Collect();
        }

        protected override void OnDataContextChanged(EventArgs e)
        {
            base.OnDataContextChanged(e);

            if (DataContext is Models.Version ver)
                Text = ver.Body;
        }

        private TextMate.Installation _textMate = null;
    }

    public partial class SelfUpdate : ChromelessWindow
    {
        public SelfUpdate()
        {
            CloseOnESC = true;
            InitializeComponent();
        }

        private void CloseWindow(object _1, RoutedEventArgs _2)
        {
            Close();
        }

        private void GotoDownload(object sender, RoutedEventArgs e)
        {
            // MyGitGUI: GitO releases
            if (sender is Button { DataContext: Models.Version ver })
                Native.OS.OpenBrowser($"{Models.AppUpdater.RepositoryUrl}/releases/tag/{ver.TagName}");
            else
                Native.OS.OpenBrowser($"{Models.AppUpdater.RepositoryUrl}/releases/latest");
            e.Handled = true;
        }

        // MyGitGUI: download the update with Velopack, then GitO closes, installs it and restarts.
        private async void UpdateNow(object sender, RoutedEventArgs e)
        {
            e.Handled = true;
            if (sender is not Button { DataContext: Models.Version ver } btn)
                return;

            var status = btn.FindNameScope()?.Find<TextBlock>("TxtUpdateStatus");
            void ShowStatus(string text)
            {
                if (status != null)
                {
                    status.Text = text;
                    status.IsVisible = true;
                }
            }

            btn.IsEnabled = false;
            ShowStatus("Downloading...");

            try
            {
                await Models.AppUpdater.DownloadAndRestartAsync(ver, p => Dispatcher.UIThread.Post(() => ShowStatus($"Downloading... {p}%")));
            }
            catch (Exception ex)
            {
                ShowStatus($"Update failed: {ex.Message}");
                btn.IsEnabled = true;
            }
        }

        private void IgnoreThisVersion(object sender, RoutedEventArgs e)
        {
            if (sender is Button { DataContext: Models.Version ver })
                ViewModels.Preferences.Instance.IgnoreUpdateTag = ver.TagName;

            Close();
            e.Handled = true;
        }
    }
}
