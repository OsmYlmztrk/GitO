using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace SourceGit.Models
{
    /// <summary>
    ///     MyGitGUI: readable name of an actor file of a One File Per Actor level
    ///     (Content/__ExternalActors__/&lt;level&gt;/X/YY/&lt;hash&gt;.uasset). Unreal writes the actor's label and class
    ///     into the package's asset registry tags (ActorLabel, ActorMetaDataClass) as plain FStrings, so they are read
    ///     straight from the file, without Unreal.
    /// </summary>
    public class UnrealActorInfo
    {
        public string Label { get; }
        public string ClassName { get; }
        public string Level { get; }
        public string ActorsFolder { get; }

        public string Detail
        {
            get => string.IsNullOrEmpty(ClassName) ? Level : $"{ClassName} · {Level}";
        }

        private UnrealActorInfo(string label, string className, string actorsFolder)
        {
            Label = label;
            ClassName = className;
            ActorsFolder = actorsFolder;
            Level = actorsFolder.Substring(actorsFolder.LastIndexOf('/') + 1);
        }

        /// <summary>
        ///     Content/__ExternalActors__/NewMap/D/YX/KDQ.uasset -> Content/__ExternalActors__/NewMap. Null for other files.
        /// </summary>
        public static string GetActorsFolder(string path)
        {
            if (!path.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase))
                return null;

            int levelStart;
            if (path.StartsWith(Marker, StringComparison.Ordinal))
            {
                levelStart = Marker.Length;
            }
            else
            {
                var marker = path.IndexOf("/" + Marker, StringComparison.Ordinal);
                if (marker < 0)
                    return null;
                levelStart = marker + 1 + Marker.Length;
            }

            // <level path>/<hash dir>/<hash dir>/<file>: drop the last three segments.
            var end = path.Length;
            for (var i = 0; i < 3; i++)
            {
                end = path.LastIndexOf('/', end - 1);
                if (end <= levelStart)
                    return null;
            }

            return path.Substring(0, end);
        }

        /// <summary>
        ///     Sets Change.UnrealActor for the actor files among the local changes. Runs off the UI thread; reads the
        ///     working file, or the local LFS object when the file is a pointer (conflicts) or gone (deletions).
        /// </summary>
        public static void Fill(string repo, string gitDir, IList<Change> changes)
        {
            foreach (var c in changes)
            {
                var folder = GetActorsFolder(c.Path);
                if (folder != null)
                    c.UnrealActor = Read(repo, gitDir, c.Path, folder);
            }
        }

        private static UnrealActorInfo Read(string repo, string gitDir, string path, string folder)
        {
            try
            {
                var full = Path.Combine(repo, path);
                byte[] data = null;
                string key;

                var file = new FileInfo(full);
                if (file.Exists)
                {
                    key = $"{path}|{file.Length}|{file.LastWriteTimeUtc.Ticks}";
                    if (Cache.TryGetValue(key, out var cached))
                        return cached;

                    data = File.ReadAllBytes(full);
                    if (IsPointer(data))
                        data = ReadLFSObject(gitDir, GetFirstOid(data));
                }
                else
                {
                    // Deleted here: the label comes from the last committed version.
                    var oid = GetFirstOid(ReadFromHead(repo, path));
                    key = $"oid|{oid}";
                    if (Cache.TryGetValue(key, out var cached))
                        return cached;

                    data = ReadLFSObject(gitDir, oid);
                }

                var info = data != null ? Parse(data, folder) : null;
                Cache[key] = info;
                return info;
            }
            catch
            {
                return null;
            }
        }

        private static UnrealActorInfo Parse(byte[] data, string folder)
        {
            var (label, labelAt) = FindTag(data, "ActorLabel");
            var (classPath, classAt) = FindTag(data, "ActorMetaDataClass");
            if (classPath == null)
                classPath = FindClassBefore(data, labelAt);
            if (label == null && classPath == null)
                return null;

            var className = classPath;
            if (className != null)
            {
                className = className.Substring(className.LastIndexOf('.') + 1);
                if (className.EndsWith("_C", StringComparison.Ordinal))
                    className = className.Substring(0, className.Length - 2);
            }

            return new UnrealActorInfo(label ?? className, className, folder);
        }

        /// <summary>
        ///     A tag is the key FString directly followed by the value FString. The same key also sits in the name
        ///     table, followed by a hash instead, so occurrences without a valid value are skipped.
        /// </summary>
        private static (string, int) FindTag(byte[] data, string key)
        {
            var pattern = new byte[4 + key.Length + 1];
            BitConverter.TryWriteBytes(pattern.AsSpan(0, 4), key.Length + 1);
            Encoding.ASCII.GetBytes(key, 0, key.Length, pattern, 4);

            var span = data.AsSpan();
            var start = 0;
            while (start < data.Length)
            {
                var found = span.Slice(start).IndexOf(pattern);
                if (found < 0)
                    break;

                var at = start + found;
                if (TryReadFString(data, at + pattern.Length, out var value, out _))
                    return (value, at);

                start = at + 1;
            }

            return (null, -1);
        }

        /// <summary>
        ///     Older files without ActorMetaDataClass: the asset's class path is the last "/Module.Class" FString
        ///     before its tags.
        /// </summary>
        private static string FindClassBefore(byte[] data, int tagsAt)
        {
            if (tagsAt <= 0)
                return null;

            for (var p = tagsAt - 4; p >= Math.Max(0, tagsAt - 1024); p--)
            {
                if (TryReadFString(data, p, out var value, out var end) && end <= tagsAt && value.StartsWith('/') && value.Contains('.'))
                    return value;
            }

            return null;
        }

        private static bool TryReadFString(byte[] data, int at, out string value, out int end)
        {
            value = null;
            end = at;
            if (at < 0 || at + 4 > data.Length)
                return false;

            var length = BitConverter.ToInt32(data, at);
            if (length > 0 && length < 1024)
            {
                if (at + 4 + length > data.Length || data[at + 4 + length - 1] != 0)
                    return false;

                for (var i = at + 4; i < at + 4 + length - 1; i++)
                {
                    if (data[i] < 32 || data[i] > 126)
                        return false;
                }

                value = Encoding.ASCII.GetString(data, at + 4, length - 1);
                end = at + 4 + length;
                return true;
            }

            // Negative length: UTF-16, e.g. labels with Turkish letters.
            if (length < 0 && length > -1024)
            {
                var bytes = -length * 2;
                if (at + 4 + bytes > data.Length || data[at + 4 + bytes - 1] != 0 || data[at + 4 + bytes - 2] != 0)
                    return false;

                value = Encoding.Unicode.GetString(data, at + 4, bytes - 2);
                end = at + 4 + bytes;
                return value.Length > 0 && !value.Contains('\0');
            }

            return false;
        }

        private static bool IsPointer(byte[] data)
        {
            return data.Length < 1024 && data.AsSpan().StartsWith("version https://git-lfs"u8);
        }

        /// <summary>
        ///     The first oid of a pointer. In a conflicted pointer that is ours (the HEAD side).
        /// </summary>
        private static string GetFirstOid(byte[] pointer)
        {
            if (pointer == null)
                return null;

            var text = Encoding.ASCII.GetString(pointer);
            var at = text.IndexOf("oid sha256:", StringComparison.Ordinal);
            if (at < 0 || at + 11 + 64 > text.Length)
                return null;

            return text.Substring(at + 11, 64);
        }

        private static byte[] ReadLFSObject(string gitDir, string oid)
        {
            if (string.IsNullOrEmpty(oid))
                return null;

            var file = Path.Combine(gitDir, "lfs", "objects", oid.Substring(0, 2), oid.Substring(2, 2), oid);
            return File.Exists(file) ? File.ReadAllBytes(file) : null;
        }

        private static byte[] ReadFromHead(string repo, string path)
        {
            var starter = new ProcessStartInfo(Native.OS.GitExecutable, $"show HEAD:{path.Quoted()}")
            {
                WorkingDirectory = repo,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
            };

            using var proc = Process.Start(starter);
            if (proc == null)
                return null;

            using var stream = new MemoryStream();
            proc.StandardOutput.BaseStream.CopyTo(stream);
            proc.WaitForExit();
            return proc.ExitCode == 0 ? stream.ToArray() : null;
        }

        private const string Marker = "__ExternalActors__/";
        private static readonly ConcurrentDictionary<string, UnrealActorInfo> Cache = new();
    }
}
