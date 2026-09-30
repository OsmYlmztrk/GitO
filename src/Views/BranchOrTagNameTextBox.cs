using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Templates;
using Avalonia.Input;
using Avalonia.Input.Platform;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.LogicalTree;
using Avalonia.Media;
using Avalonia.VisualTree;

namespace SourceGit.Views
{
    public class BranchOrTagNameTextBox : TextBox
    {
        /// <summary>
        ///     MyGitGUI: suggest existing folder prefixes (e.g. feature/, bugfix/) taken from
        ///     local and remote branches (or tags in the Create Tag dialog).
        /// </summary>
        public static readonly StyledProperty<bool> IsFolderSuggestionEnabledProperty =
            AvaloniaProperty.Register<BranchOrTagNameTextBox, bool>(nameof(IsFolderSuggestionEnabled), true);

        public bool IsFolderSuggestionEnabled
        {
            get => GetValue(IsFolderSuggestionEnabledProperty);
            set => SetValue(IsFolderSuggestionEnabledProperty, value);
        }

        protected override Type StyleKeyOverride => typeof(TextBox);

        protected override void OnLoaded(RoutedEventArgs e)
        {
            base.OnLoaded(e);
            PastingFromClipboard += OnPastingFromClipboard;
            TextChanged += OnTextChangedForSuggestions;
        }

        protected override void OnUnloaded(RoutedEventArgs e)
        {
            PastingFromClipboard -= OnPastingFromClipboard;
            TextChanged -= OnTextChangedForSuggestions;
            CloseSuggestions();
            base.OnUnloaded(e);
        }

        protected override void OnGotFocus(GotFocusEventArgs e)
        {
            base.OnGotFocus(e);
            UpdateSuggestions();
        }

        protected override void OnLostFocus(RoutedEventArgs e)
        {
            base.OnLostFocus(e);
            CloseSuggestions();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (_popup is { IsOpen: true } && _list != null && e.KeyModifiers == KeyModifiers.None)
            {
                var count = _list.ItemCount;
                switch (e.Key)
                {
                    case Key.Down:
                        _list.SelectedIndex = count == 0 ? -1 : (_list.SelectedIndex + 1) % count;
                        _list.ScrollIntoView(_list.SelectedIndex);
                        SyncEnterSteal();
                        e.Handled = true;
                        return;
                    case Key.Up:
                        _list.SelectedIndex = count == 0 ? -1 : (_list.SelectedIndex <= 0 ? count - 1 : _list.SelectedIndex - 1);
                        _list.ScrollIntoView(_list.SelectedIndex);
                        SyncEnterSteal();
                        e.Handled = true;
                        return;
                    case Key.Tab:
                    case Key.Enter:
                        if (_list.SelectedItem is Suggestion s)
                        {
                            Accept(s);
                            e.Handled = true;
                            return;
                        }
                        break;
                    case Key.Escape:
                        CloseSuggestions();
                        e.Handled = true;
                        return;
                }
            }
            else if (e.Key == Key.Down && e.KeyModifiers == KeyModifiers.None && IsFolderSuggestionEnabled)
            {
                UpdateSuggestions();
                e.Handled = _popup is { IsOpen: true };
                if (e.Handled)
                    return;
            }

            base.OnKeyDown(e);
        }

        protected override void OnTextInput(TextInputEventArgs e)
        {
            if (string.IsNullOrEmpty(e.Text))
                return;

            var builder = new StringBuilder(e.Text.Length);
            var chars = e.Text.ToCharArray();
            foreach (var ch in chars)
            {
                if (char.IsWhiteSpace(ch))
                    builder.Append('-');
                else
                    builder.Append(ch);
            }

            e.Text = builder.ToString();
            base.OnTextInput(e);
        }

        private async void OnPastingFromClipboard(object sender, RoutedEventArgs e)
        {
            e.Handled = true;

            try
            {
                var clipboard = TopLevel.GetTopLevel(this)?.Clipboard;
                if (clipboard != null)
                {
                    var text = await clipboard.TryGetTextAsync();
                    if (!string.IsNullOrEmpty(text))
                        OnTextInput(new TextInputEventArgs() { Text = text });
                }
            }
            catch
            {
                // Ignore exceptions
            }
        }

        private void OnTextChangedForSuggestions(object sender, TextChangedEventArgs e)
        {
            if (_isAccepting || !IsKeyboardFocusWithin)
                return;

            UpdateSuggestions();
        }

        private record Suggestion(string Result, string Folder, int Count, bool IsPrepend);

        private void UpdateSuggestions()
        {
            if (!IsFolderSuggestionEnabled)
            {
                CloseSuggestions();
                return;
            }

            var folders = CollectFolders();
            if (folders.Count == 0)
            {
                CloseSuggestions();
                return;
            }

            var text = Text ?? string.Empty;
            var lastSlash = text.LastIndexOf('/');
            var depth = text.Count(c => c == '/');
            var items = new List<Suggestion>();

            // Next-level folders that continue what has been typed so far.
            foreach (var (folder, count) in folders)
            {
                if (folder.Count(c => c == '/') != depth + 1)
                    continue;
                if (!folder.StartsWith(text, StringComparison.OrdinalIgnoreCase) || folder.Length == text.Length)
                    continue;

                items.Add(new Suggestion(folder, folder, count, false));
            }

            // A plain name without any folder: offer to put it into an existing top-level folder.
            if (items.Count == 0 && lastSlash < 0 && text.Length > 0)
            {
                foreach (var (folder, count) in folders)
                {
                    if (folder.Count(c => c == '/') == 1)
                        items.Add(new Suggestion(folder + text, folder, count, true));
                }
            }

            if (items.Count == 0)
            {
                CloseSuggestions();
                return;
            }

            items.Sort((l, r) => r.Count != l.Count ? r.Count.CompareTo(l.Count) : string.Compare(l.Folder, r.Folder, StringComparison.OrdinalIgnoreCase));
            ShowSuggestions(items);
        }

        private List<(string, int)> CollectFolders()
        {
            var names = new List<string>();
            var page = this.FindAncestorOfType<LauncherPage>()?.DataContext as ViewModels.LauncherPage;
            if (page?.Data is ViewModels.Repository repo)
            {
                if (DataContext is ViewModels.CreateTag)
                {
                    foreach (var t in repo.Tags)
                        names.Add(t.Name);
                }
                else
                {
                    foreach (var b in repo.Branches)
                        names.Add(b.Name);
                }
            }

            // Count distinct branch names under each folder, at every depth (feature/, feature/ui/, ...).
            var counts = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
            foreach (var name in names)
            {
                var idx = name.IndexOf('/');
                while (idx > 0)
                {
                    var folder = name.Substring(0, idx + 1);
                    if (!counts.TryGetValue(folder, out var set))
                    {
                        set = new HashSet<string>(StringComparer.Ordinal);
                        counts.Add(folder, set);
                    }

                    set.Add(name);
                    idx = name.IndexOf('/', idx + 1);
                }
            }

            return counts.Select(kv => (kv.Key, kv.Value.Count)).ToList();
        }

        private void ShowSuggestions(List<Suggestion> items)
        {
            EnsurePopup();

            _list.ItemsSource = items;
            _list.SelectedIndex = -1;
            _popup.Width = Math.Max(Bounds.Width, 240);
            _header.Text = items[0].IsPrepend ? "Put it in an existing folder" : "Existing folders";
            _popup.IsOpen = true;
            SyncEnterSteal();
        }

        private void CloseSuggestions()
        {
            if (_popup != null)
                _popup.IsOpen = false;

            SyncEnterSteal();
        }

        private void Accept(Suggestion s)
        {
            _isAccepting = true;
            Text = s.Result;
            CaretIndex = Text.Length;
            _isAccepting = false;

            // Keep going: show sub-folders of the chosen folder, if any.
            if (s.IsPrepend)
                CloseSuggestions();
            else
                UpdateSuggestions();

            Focus();
        }

        // While a suggestion is highlighted, Enter picks it instead of confirming the dialog.
        private void SyncEnterSteal()
        {
            var steal = _popup is { IsOpen: true } && _list?.SelectedItem != null;
            if (steal)
            {
                if (Tag == null)
                {
                    Tag = StealHotKey.Enter;
                    _ownsTag = true;
                }
            }
            else if (_ownsTag)
            {
                Tag = null;
                _ownsTag = false;
            }
        }

        private void EnsurePopup()
        {
            if (_popup != null)
                return;

            _header = new TextBlock()
            {
                Margin = new Thickness(10, 6, 10, 4),
                FontSize = 11,
                FontWeight = FontWeight.SemiBold,
            };
            _header.Bind(TextBlock.ForegroundProperty, this.GetResourceObservable("Brush.FG2"));

            _list = new ListBox()
            {
                Focusable = false,
                MaxHeight = 220,
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                SelectionMode = SelectionMode.Single,
                ItemTemplate = new FuncDataTemplate<Suggestion>((s, _) =>
                {
                    if (s == null)
                        return null;

                    var grid = new Grid()
                    {
                        ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"),
                        Height = 26,
                        Background = Brushes.Transparent,
                    };

                    var icon = new Avalonia.Controls.Shapes.Path()
                    {
                        Width = 12,
                        Height = 12,
                        Stretch = Stretch.Uniform,
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    icon.Bind(Avalonia.Controls.Shapes.Path.DataProperty, this.GetResourceObservable("Icons.Folder"));
                    icon.Bind(Avalonia.Controls.Shapes.Shape.FillProperty, this.GetResourceObservable("Brush.FG2"));

                    var label = new TextBlock()
                    {
                        Margin = new Thickness(8, 0, 8, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                    };
                    if (s.IsPrepend)
                    {
                        label.Inlines = new Avalonia.Controls.Documents.InlineCollection
                        {
                            new Avalonia.Controls.Documents.Run(s.Folder) { FontWeight = FontWeight.SemiBold },
                            new Avalonia.Controls.Documents.Run(s.Result.Substring(s.Folder.Length)),
                        };
                    }
                    else
                    {
                        label.Text = s.Folder;
                    }

                    var count = new TextBlock()
                    {
                        Text = s.Count == 1 ? "1 branch" : $"{s.Count} branches",
                        FontSize = 11,
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    count.Bind(TextBlock.ForegroundProperty, this.GetResourceObservable("Brush.FG2"));

                    Grid.SetColumn(icon, 0);
                    Grid.SetColumn(label, 1);
                    Grid.SetColumn(count, 2);
                    grid.Children.Add(icon);
                    grid.Children.Add(label);
                    grid.Children.Add(count);
                    grid.PointerPressed += (_, ev) =>
                    {
                        Accept(s);
                        ev.Handled = true;
                    };
                    return grid;
                }),
            };
            var itemStyle = new Avalonia.Styling.Style(x => Avalonia.Styling.Selectors.OfType<ListBoxItem>(x));
            itemStyle.Setters.Add(new Avalonia.Styling.Setter(ListBoxItem.PaddingProperty, new Thickness(10, 0)));
            itemStyle.Setters.Add(new Avalonia.Styling.Setter(ListBoxItem.MinHeightProperty, 0.0));
            _list.Styles.Add(itemStyle);
            _list.SelectionChanged += (_, _) => SyncEnterSteal();

            var panel = new StackPanel() { Orientation = Orientation.Vertical };
            panel.Children.Add(_header);
            panel.Children.Add(_list);

            var border = new Border()
            {
                Padding = new Thickness(0, 0, 0, 4),
                CornerRadius = new CornerRadius(6),
                BorderThickness = new Thickness(1),
                BoxShadow = BoxShadows.Parse("0 4 12 0 #60000000"),
                Child = panel,
            };
            border.Bind(Border.BackgroundProperty, this.GetResourceObservable("Brush.Popup"));
            border.Bind(Border.BorderBrushProperty, this.GetResourceObservable("Brush.Border2"));

            _popup = new Popup()
            {
                Child = border,
                PlacementTarget = this,
                Placement = PlacementMode.BottomEdgeAlignedLeft,
                VerticalOffset = 2,
                IsLightDismissEnabled = false,
                Focusable = false,
            };

            LogicalChildren.Add(_popup);
        }

        private Popup _popup = null;
        private ListBox _list = null;
        private TextBlock _header = null;
        private bool _isAccepting = false;
        private bool _ownsTag = false;
    }
}
