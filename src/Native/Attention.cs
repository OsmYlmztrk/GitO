using System;
using System.Runtime.InteropServices;

namespace SourceGit.Native
{
    /// <summary>
    ///     MyGitGUI: get the user's attention (system sound, flashing taskbar button). Windows only; no-op elsewhere.
    /// </summary>
    public static class Attention
    {
        public static void PlayAlert(bool important)
        {
            if (!OperatingSystem.IsWindows())
                return;

            try
            {
                PlaySound(important ? "SystemExclamation" : "SystemAsterisk", IntPtr.Zero, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            }
            catch
            {
                // Sound is best effort.
            }
        }

        public static void FlashTaskbar(IntPtr hwnd)
        {
            if (!OperatingSystem.IsWindows() || hwnd == IntPtr.Zero)
                return;

            try
            {
                var info = new FLASHWINFO
                {
                    cbSize = (uint)Marshal.SizeOf<FLASHWINFO>(),
                    hwnd = hwnd,
                    dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG,
                    uCount = 0,
                    dwTimeout = 0,
                };
                FlashWindowEx(ref info);
            }
            catch
            {
                // Best effort.
            }
        }

        private const uint SND_ASYNC = 0x0001;
        private const uint SND_NODEFAULT = 0x0002;
        private const uint SND_ALIAS = 0x00010000;
        private const uint FLASHW_ALL = 0x00000003;
        private const uint FLASHW_TIMERNOFG = 0x0000000C;

        [StructLayout(LayoutKind.Sequential)]
        private struct FLASHWINFO
        {
            public uint cbSize;
            public IntPtr hwnd;
            public uint dwFlags;
            public uint uCount;
            public uint dwTimeout;
        }

        [DllImport("winmm.dll", EntryPoint = "PlaySoundW", CharSet = CharSet.Unicode)]
        private static extern bool PlaySound(string sound, IntPtr module, uint flags);

        [DllImport("user32.dll")]
        private static extern bool FlashWindowEx(ref FLASHWINFO info);
    }
}
