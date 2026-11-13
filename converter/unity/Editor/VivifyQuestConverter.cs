// Vivify Quest converter for the Unity Editor.
//
// Turns a PC Vivify bundle (bundleWindows2021.vivify / bundleWindows2019.vivify)
// into bundleAndroid2021.vivify, the file the Vivify Quest mod loads first, so a
// map can ship a Quest bundle converted ahead of time instead of being
// converted on the headset. It is the converter the mod itself runs, built for
// the desktop (vivifyconvert.dll / libvivifyconvert.so / libvivifyconvert.dylib).
//
// Install: copy this folder (Editor/) and the native library for your
// platform into your Unity project, e.g.
//   Assets/VivifyQuestConverter/Editor/VivifyQuestConverter.cs
//   Assets/VivifyQuestConverter/Editor/Plugins/vivifyconvert.dll
// and, in the library's import settings, tick Editor only.
//
// Use: Vivify > Quest Converter (window), or from a build script:
//   var result = VivifyQuestConverter.ConvertForQuest("Build/bundleWindows2021.vivify");
// which writes Build/bundleAndroid2021.vivify.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace VivifyQuest.Converter
{
    public static class VivifyQuestConverter
    {
        const string Library = "vivifyconvert";
        public const string QuestBundleName = "bundleAndroid2021.vivify";
        // The C header's VIVIFY_CONVERT_API_VERSION this file was written for.
        const int ApiVersion = 1;

        public enum Status
        {
            Success = 0,
            AlreadyAndroid = 1,
            SourceUnreadable = 2,
            NotAUnityBundle = 3,
            UnsupportedCompression = 4,
            Corrupt = 5,
            DestUnwritable = 6,
            OutOfMemory = 7,
            BadArgument = 100,
            LibraryMissing = 1000,
        }

        // Mirrors VivifyConvertOptions in vivify_convert.h, field for field.
        [StructLayout(LayoutKind.Sequential)]
        public struct Options
        {
            public uint structSize;
            public int translateShaders;
            public int separateStereoVariants;
            public int splitUnity2019;
            public int singleViewOnly;
            public int renameArchiveFiles;
            public int dropUntranslatableGeometry;
            public int dropGeometryStages;
            public int geometryRightEyeCopy;
            public int skipQuestUnusedVariants;

            // What the Vivify Quest mod converts a map's PC bundle with.
            public static Options Default
            {
                get
                {
                    var options = new Options();
                    try
                    {
                        vivify_convert_default_options(ref options);
                    }
                    catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException)
                    {
                        // The library's defaults, for when it is not there (Convert then reports why).
                        options.structSize = (uint)Marshal.SizeOf<Options>();
                        options.translateShaders = 1;
                        options.geometryRightEyeCopy = 1;
                        options.skipQuestUnusedVariants = 1;
                    }
                    return options;
                }
            }
        }

        // Mirrors VivifyConvertResult in vivify_convert.h, field for field.
        [StructLayout(LayoutKind.Sequential)]
        public struct Counts
        {
            public uint structSize;
            public int status;
            public int shadersSeen;
            public int shadersTranslated;
            public int shadersLeftAlone;
            public int shadersRefused;
            public int programsTranslated;
            public int variantsLinked;
            public int variantsRefused;
            public int variantsSkipped;
            public int geometryStagesDropped;
            public int texturesSeen;
            public int texturesMarkedReadable;
            public ulong outputBytes;
        }

        public sealed class Result
        {
            public Status status;
            public Counts counts;
            // The converter's summary, or why it failed.
            public string message = "";

            public bool Succeeded => status == Status.Success || status == Status.AlreadyAndroid;
        }

        [DllImport(Library)] static extern int vivify_convert_api_version();
        [DllImport(Library)] static extern int vivify_convert_revision();
        [DllImport(Library)] static extern void vivify_convert_default_options(ref Options options);
        [DllImport(Library)] static extern int vivify_convert_bundle(byte[] source, byte[] dest, ref Options options,
                                                                      ref Counts result, byte[] message, UIntPtr messageSize);
        [DllImport(Library)] static extern UIntPtr vivify_convert_describe_bundle(byte[] path, byte[] text, UIntPtr textSize);
        [DllImport(Library)] static extern int vivify_convert_is_bundle(byte[] path);

        // Paths cross as NUL-terminated UTF-8, on every platform and runtime.
        static byte[] Utf8(string text) => Encoding.UTF8.GetBytes((text ?? string.Empty) + "\0");

        static string FromUtf8(byte[] bytes)
        {
            int length = Array.IndexOf(bytes, (byte)0);
            return Encoding.UTF8.GetString(bytes, 0, length < 0 ? bytes.Length : length);
        }

        // Null when the native library loads and matches this script; otherwise why not.
        public static string LibraryProblem()
        {
            try
            {
                int api = vivify_convert_api_version();
                return api == ApiVersion ? null : $"the native library has API {api}, this script expects {ApiVersion}";
            }
            catch (DllNotFoundException)
            {
                return $"the native library '{Library}' was not found; put it beside this script under Editor/Plugins";
            }
            catch (EntryPointNotFoundException)
            {
                return $"'{Library}' is not the Vivify Quest converter";
            }
        }

        // The converter revision, as the mod's cache: a bundle converted by an
        // older revision is converted again on the headset. -1 without the library.
        public static int Revision()
        {
            return LibraryProblem() == null ? vivify_convert_revision() : -1;
        }

        public static Result Convert(string sourcePath, string destPath) => Convert(sourcePath, destPath, Options.Default);

        public static Result Convert(string sourcePath, string destPath, Options options)
        {
            var result = new Result();
            string problem = LibraryProblem();
            if (problem != null)
            {
                result.status = Status.LibraryMissing;
                result.message = problem;
                return result;
            }
            options.structSize = (uint)Marshal.SizeOf<Options>();
            var counts = new Counts { structSize = (uint)Marshal.SizeOf<Counts>() };
            var message = new byte[64 * 1024];
            result.status = (Status)vivify_convert_bundle(Utf8(Path.GetFullPath(sourcePath)), Utf8(Path.GetFullPath(destPath)),
                                                          ref options, ref counts, message, (UIntPtr)message.Length);
            result.counts = counts;
            result.message = FromUtf8(message);
            return result;
        }

        // Converts a PC bundle into bundleAndroid2021.vivify in the same folder.
        public static Result ConvertForQuest(string pcBundlePath) => ConvertForQuest(pcBundlePath, Options.Default);

        public static Result ConvertForQuest(string pcBundlePath, Options options)
        {
            string folder = Path.GetDirectoryName(Path.GetFullPath(pcBundlePath)) ?? ".";
            return Convert(pcBundlePath, Path.Combine(folder, QuestBundleName), options);
        }

        // What a bundle's shaders were compiled for: Unity version, platforms, programs.
        public static string Describe(string path)
        {
            string problem = LibraryProblem();
            if (problem != null) return problem;
            byte[] utf8 = Utf8(Path.GetFullPath(path));
            int length = (int)vivify_convert_describe_bundle(utf8, null, UIntPtr.Zero);
            var text = new byte[length + 1];
            vivify_convert_describe_bundle(utf8, text, (UIntPtr)text.Length);
            return FromUtf8(text);
        }

        public static bool IsUnityBundle(string path)
        {
            return LibraryProblem() == null && vivify_convert_is_bundle(Utf8(Path.GetFullPath(path))) != 0;
        }
    }

    public class VivifyQuestConverterWindow : EditorWindow
    {
        const string PrefsKey = "VivifyQuestConverter.";
        string _source = "";
        string _dest = "";
        VivifyQuestConverter.Options _options;
        string _report = "";
        Vector2 _scroll;

        [MenuItem("Vivify/Quest Converter")]
        static void Open() => GetWindow<VivifyQuestConverterWindow>("Vivify Quest Converter");

        [MenuItem("Vivify/Convert Bundle For Quest...")]
        static void ConvertWithDialogs()
        {
            string source = EditorUtility.OpenFilePanel("PC Vivify bundle", "", "vivify");
            if (string.IsNullOrEmpty(source)) return;
            string dest = EditorUtility.SaveFilePanel("Quest bundle", Path.GetDirectoryName(source),
                                                      VivifyQuestConverter.QuestBundleName, "vivify");
            if (string.IsNullOrEmpty(dest)) return;
            var result = Run(source, dest, VivifyQuestConverter.Options.Default);
            EditorUtility.DisplayDialog("Vivify Quest Converter", Summary(result), "OK");
        }

        void OnEnable()
        {
            _options = VivifyQuestConverter.Options.Default;
            _source = EditorPrefs.GetString(PrefsKey + "source", "");
            _dest = EditorPrefs.GetString(PrefsKey + "dest", "");
        }

        void OnGUI()
        {
            string problem = VivifyQuestConverter.LibraryProblem();
            if (problem != null)
            {
                EditorGUILayout.HelpBox(problem, MessageType.Error);
                return;
            }
            EditorGUILayout.LabelField($"Converter revision {VivifyQuestConverter.Revision()}", EditorStyles.miniLabel);

            _source = PathField("PC bundle", _source, () => EditorUtility.OpenFilePanel("PC Vivify bundle", Folder(_source), "vivify"));
            if (GUILayout.Button("Quest bundle beside it (" + VivifyQuestConverter.QuestBundleName + ")") && _source.Length > 0)
                _dest = Path.Combine(Folder(_source), VivifyQuestConverter.QuestBundleName);
            _dest = PathField("Quest bundle", _dest, () => EditorUtility.SaveFilePanel("Quest bundle", Folder(_dest.Length > 0 ? _dest : _source),
                                                                                         VivifyQuestConverter.QuestBundleName, "vivify"));

            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Options (the defaults are what the mod uses)", EditorStyles.boldLabel);
            _options.translateShaders = Toggle("Translate shaders", "Off: only retarget the bundle to Android.", _options.translateShaders);
            using (new EditorGUI.DisabledScope(_options.translateShaders == 0))
            {
                _options.geometryRightEyeCopy = Toggle("Geometry effects in right eye",
                    "Geometry stages also draw their output into the right eye.", _options.geometryRightEyeCopy);
                _options.dropGeometryStages = Toggle("Flat geometry effects",
                    "Draw geometry-shader variants without that stage.", _options.dropGeometryStages);
                _options.dropUntranslatableGeometry = Toggle("Flatten untranslatable geometry",
                    "Only variants whose geometry stage cannot be translated.", _options.dropUntranslatableGeometry);
                _options.skipQuestUnusedVariants = Toggle("Skip variants Quest never uses",
                    "GPU instancing and unsplit stereo. Turn off if your materials keep GPU instancing on.", _options.skipQuestUnusedVariants);
                _options.singleViewOnly = Toggle("Single view only",
                    "For cameras that draw into plain textures only.", _options.singleViewOnly);
            }
            if (GUILayout.Button("Reset to defaults")) _options = VivifyQuestConverter.Options.Default;

            EditorGUILayout.Space();
            using (new EditorGUI.DisabledScope(_source.Length == 0 || _dest.Length == 0))
            {
                if (GUILayout.Button("Convert", GUILayout.Height(28)))
                {
                    EditorPrefs.SetString(PrefsKey + "source", _source);
                    EditorPrefs.SetString(PrefsKey + "dest", _dest);
                    _report = Summary(Run(_source, _dest, _options));
                }
                if (GUILayout.Button("Describe PC bundle"))
                    _report = VivifyQuestConverter.Describe(_source);
            }
            _scroll = EditorGUILayout.BeginScrollView(_scroll);
            EditorGUILayout.TextArea(_report, EditorStyles.wordWrappedLabel);
            EditorGUILayout.EndScrollView();
        }

        static VivifyQuestConverter.Result Run(string source, string dest, VivifyQuestConverter.Options options)
        {
            try
            {
                EditorUtility.DisplayProgressBar("Vivify Quest Converter", "Converting " + Path.GetFileName(source), 0.5f);
                var result = VivifyQuestConverter.Convert(source, dest, options);
                if (result.Succeeded) Debug.Log("[Vivify Quest] " + Summary(result));
                else Debug.LogError("[Vivify Quest] " + Summary(result));
                return result;
            }
            finally
            {
                EditorUtility.ClearProgressBar();
            }
        }

        static string Summary(VivifyQuestConverter.Result result)
        {
            if (!result.Succeeded) return $"Failed ({result.status}): {result.message}";
            var c = result.counts;
            return $"{result.status}: {result.message}\n\nShaders: {c.shadersTranslated} of {c.shadersSeen} translated, "
                 + $"{c.shadersRefused} refused. Variants: {c.variantsLinked} linked, {c.variantsRefused} refused, "
                 + $"{c.variantsSkipped} skipped. {c.outputBytes:N0} bytes.";
        }

        static string Folder(string path) => string.IsNullOrEmpty(path) ? "" : Path.GetDirectoryName(path);

        static int Toggle(string label, string tip, int value) =>
            EditorGUILayout.Toggle(new GUIContent(label, tip), value != 0) ? 1 : 0;

        static string PathField(string label, string value, Func<string> browse)
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                value = EditorGUILayout.TextField(label, value);
                if (GUILayout.Button("...", GUILayout.Width(28)))
                {
                    string chosen = browse();
                    if (!string.IsNullOrEmpty(chosen)) value = chosen;
                }
            }
            return value;
        }
    }
}
