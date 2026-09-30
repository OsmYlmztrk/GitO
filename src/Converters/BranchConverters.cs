using System.Collections.Generic;
using Avalonia;
using Avalonia.Data.Converters;

namespace SourceGit.Converters
{
    public static class BranchConverters
    {
        /// <summary>
        ///     MyGitGUI: brush of the commit-graph lane that a branch head sits on.
        ///     Values: [branch (object), head SHA -> lane color index, enabled (bool)].
        ///     Falls back to the theme's normal text brush when disabled or the head is not in the loaded graph.
        /// </summary>
        public static readonly FuncMultiValueConverter<object, object> ToGraphBrush =
            new(values =>
            {
                object branch = null;
                IReadOnlyDictionary<string, int> colors = null;
                var enabled = false;

                var i = 0;
                foreach (var v in values)
                {
                    switch (i++)
                    {
                        case 0: branch = v; break;
                        case 1: colors = v as IReadOnlyDictionary<string, int>; break;
                        case 2: enabled = v is true; break;
                    }
                }

                if (!enabled || colors == null || branch is not Models.Branch b || string.IsNullOrEmpty(b.Head))
                    return DefaultBrush();

                var pens = Models.CommitGraph.Pens;
                if (pens.Count == 0 || !colors.TryGetValue(b.Head, out var color))
                    return DefaultBrush();

                return pens[color % pens.Count].Brush;
            });

        // Returning UnsetValue from a MultiBinding leaves the text with a dark fallback color,
        // so hand back the theme's regular foreground brush explicitly.
        private static object DefaultBrush()
        {
            var app = Application.Current;
            if (app != null && app.TryGetResource("Brush.FG1", app.ActualThemeVariant, out var brush))
                return brush;

            return AvaloniaProperty.UnsetValue;
        }
    }
}
