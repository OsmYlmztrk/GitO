using System;

using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace SourceGit.Views
{
    public class CommitStatusIndicator : Control
    {
        public static readonly DirectProperty<CommitStatusIndicator, Models.Branch> CurrentBranchProperty =
            AvaloniaProperty.RegisterDirect<CommitStatusIndicator, Models.Branch>(
                nameof(CurrentBranch),
                static o => o.CurrentBranch,
                static (o, v) => o.CurrentBranch = v);

        public Models.Branch CurrentBranch
        {
            get => _currentBranch;
            set => SetAndRaise(CurrentBranchProperty, ref _currentBranch, value);
        }

        public static readonly StyledProperty<IBrush> AheadBrushProperty =
            AvaloniaProperty.Register<CommitStatusIndicator, IBrush>(nameof(AheadBrush));

        public IBrush AheadBrush
        {
            get => GetValue(AheadBrushProperty);
            set => SetValue(AheadBrushProperty, value);
        }

        public static readonly StyledProperty<IBrush> BehindBrushProperty =
            AvaloniaProperty.Register<CommitStatusIndicator, IBrush>(nameof(BehindBrush));

        public IBrush BehindBrush
        {
            get => GetValue(BehindBrushProperty);
            set => SetValue(BehindBrushProperty, value);
        }

        private enum Status
        {
            Normal,
            Ahead,
            Behind,
        }

        // MyGitGUI: draw a 14px badge with an arrow instead of a 5px dot, so unpushed (filled ↑)
        // and unpulled (outlined ↓) commits stand out in the history list.
        public override void Render(DrawingContext context)
        {
            if (_status == Status.Normal)
                return;

            var box = new Rect(0.5, 0.5, BadgeSize - 1, BadgeSize - 1);
            var mid = BadgeSize * 0.5;
            if (_status == Status.Ahead)
            {
                context.DrawRectangle(AheadBrush, null, box, 3, 3);

                var pen = new Pen(Brushes.White, 1.5, null, PenLineCap.Round, PenLineJoin.Round);
                context.DrawLine(pen, new Point(mid, 3.5), new Point(mid, 10.5));
                context.DrawLine(pen, new Point(mid - 3, 6.5), new Point(mid, 3.5));
                context.DrawLine(pen, new Point(mid + 3, 6.5), new Point(mid, 3.5));
            }
            else
            {
                context.DrawRectangle(null, new Pen(BehindBrush, 1), box, 3, 3);

                var pen = new Pen(BehindBrush, 1.5, null, PenLineCap.Round, PenLineJoin.Round);
                context.DrawLine(pen, new Point(mid, 3.5), new Point(mid, 10.5));
                context.DrawLine(pen, new Point(mid - 3, 7.5), new Point(mid, 10.5));
                context.DrawLine(pen, new Point(mid + 3, 7.5), new Point(mid, 10.5));
            }
        }

        protected override Size MeasureOverride(Size availableSize)
        {
            if (DataContext is Models.Commit commit && _currentBranch != null)
            {
                var sha = commit.SHA;

                if (_currentBranch.Ahead.Contains(sha))
                    _status = Status.Ahead;
                else if (_currentBranch.Behind.Contains(sha))
                    _status = Status.Behind;
                else
                    _status = Status.Normal;
            }
            else
            {
                _status = Status.Normal;
            }

            // MyGitGUI: badge size + tooltip
            ToolTip.SetTip(this, _status switch
            {
                Status.Ahead => "Not pushed yet",
                Status.Behind => "Not pulled yet",
                _ => null,
            });

            return _status == Status.Normal ? new Size(0, 0) : new Size(BadgeSize + 4, BadgeSize);
        }

        private const double BadgeSize = 14;

        protected override void OnDataContextChanged(EventArgs e)
        {
            base.OnDataContextChanged(e);
            InvalidateMeasure();
        }

        protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
        {
            base.OnPropertyChanged(change);
            if (change.Property == CurrentBranchProperty)
                InvalidateMeasure();
        }

        private Models.Branch _currentBranch = null;
        private Status _status = Status.Normal;
    }
}
