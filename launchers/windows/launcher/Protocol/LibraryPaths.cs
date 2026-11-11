// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System.Text.Json;

namespace S2Arm.Launcher.Protocol;

/// <summary>
/// 游戏库结构 P3：玩家指定的库是入口，私有目录（%LOCALAPPDATA%\S2Arm\WindowsLauncher）只留设置、密钥和控制口文件。
/// 启动器算「库放哪」「日志 / 任务记录在哪」都只调这里（作业层同一规则：pack/jobs/core.py logs_root、layout.py relocate_default）。
/// </summary>
public static class LibraryPaths
{
    // 系统自动生成的文件（同作业层 layout.OS_JUNK / 前端 LibraryPage.IsOsJunk）：判空文件夹时不算数
    private static readonly HashSet<string> OsJunk = new(StringComparer.OrdinalIgnoreCase) { ".DS_Store", "desktop.ini", "Thumbs.db" };

    /// <summary>这个文件夹建不了游戏库（作业层 setup 会拒收「已有别的文件」）：存在、没有游戏库标识、也不像旧游戏库（catalog.json / iPhone\，同 library._looks_like_library），
    /// 里面有系统文件以外的东西。玩家把 S2Arm 的 zip 解压到了 %USERPROFILE%\S2Arm 就是这种（缺省库同名同位置，v0.2.8 热修）。</summary>
    public static bool Occupied(string folder)
    {
        if (!Directory.Exists(folder) || File.Exists(Path.Combine(folder, ".s2arm-library.json")) || File.Exists(Path.Combine(folder, "catalog.json"))
            || Directory.Exists(Path.Combine(folder, "iPhone"))) return false;
        try { return Directory.EnumerateFileSystemEntries(folder).Select(Path.GetFileName).Any(n => n is not null && !OsJunk.Contains(n) && !n.StartsWith("._", StringComparison.Ordinal)); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { return false; }   // 读不了交给作业层报真实原因
    }

    /// <summary>缺省库位置被占（Occupied）时换到旁边：「S2Arm Library」「S2Arm Library 2」…；都被占就原样返回（由作业层报原因）。</summary>
    public static string FreeDefaultLibrary(string folder)
    {
        if (!Occupied(folder)) return folder;
        foreach (var n in Enumerable.Range(1, 9))
        {
            var next = folder + " Library" + (n == 1 ? "" : " " + n);
            if (!Occupied(next)) return next;
        }
        return folder;
    }

    /// <summary>设置：玩家选的库路径（一行文字）。</summary>
    public static string PreferenceFile(string privateRoot) => Path.Combine(privateRoot, "library-location.txt");

    /// <summary>缺省私有目录：%LOCALAPPDATA%\S2Arm\WindowsLauncher。和作业层同一取法（layout.standard_private 读 LOCALAPPDATA 环境变量）。</summary>
    public static string StandardPrivateRoot =>
        Path.Combine(KnownFolder("LOCALAPPDATA", Environment.SpecialFolder.LocalApplicationData), "S2Arm", "WindowsLauncher");

    /// <summary>先读环境变量，没有再问系统。作业层（Python）的 Path.home() 读 USERPROFILE、私有目录读 LOCALAPPDATA，
    /// 前端只问系统（GetFolderPath 不看这两个变量）时，两边在改过环境变量的进程里会算出不同的目录：新缺省库搬到一处、前端去另一处找。</summary>
    internal static string KnownFolder(string variable, Environment.SpecialFolder folder) =>
        Environment.GetEnvironmentVariable(variable) is { Length: > 0 } value ? value : Environment.GetFolderPath(folder);

    /// <summary>新装的缺省库：%USERPROFILE%\S2Arm（资源管理器里看得见）；自定义数据目录（--data-root=、测试）时仍是数据目录下的 DefaultLibrary。</summary>
    public static string DefaultLibrary(string privateRoot) =>
        SamePath(privateRoot, StandardPrivateRoot)
            ? Path.Combine(KnownFolder("USERPROFILE", Environment.SpecialFolder.UserProfile), "S2Arm")
            : Path.Combine(privateRoot, "DefaultLibrary");

    /// <summary>设置里记的库路径按作业层的搬库标记（library-relocate.json，旧默认库同盘改名到新缺省位置时写）换成现在的位置：
    /// 记的是旧路径、新位置有库标识、旧位置没有，就用新位置；其余原样。</summary>
    public static string Library(string privateRoot, string saved)
    {
        if (string.IsNullOrEmpty(saved)) return saved;
        try
        {
            using var note = JsonDocument.Parse(File.ReadAllText(Path.Combine(privateRoot, "library-relocate.json")));
            var root = note.RootElement;
            if (root.ValueKind == JsonValueKind.Object &&
                root.TryGetProperty("from", out var from) && from.ValueKind == JsonValueKind.String &&
                root.TryGetProperty("to", out var to) && to.ValueKind == JsonValueKind.String &&
                SamePath(from.GetString()!, saved) && Built(to.GetString()!) && !Built(saved))
                return to.GetString()!;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException or NotSupportedException) { }
        return saved;
    }

    /// <summary>私有目录下的旧缺省库（0.1.x 没选过位置的玩家的库就在这里；作业层 layout.relocate_default 认这个路径才搬）。</summary>
    public static string LegacyDefaultLibrary(string privateRoot) => Path.Combine(privateRoot, "DefaultLibrary");

    /// <summary>设置文件里的库路径（已按搬库标记换过）；没有设置就是缺省库。</summary>
    public static string SavedLibrary(string privateRoot) => SavedLibrary(privateRoot, DefaultLibrary(privateRoot));

    /// <summary>同上，缺省库由调用方给（单测用：标准私有目录的缺省库在用户主目录，测不到）。
    /// 没有设置文件时先认旧缺省库：0.1.x 老玩家从没选过位置、也就没有 library-location.txt，库还在私有目录下的 DefaultLibrary。
    /// 把这个旧路径交给作业层，relocate_default 才会搬：新位置空着就同盘改名过去，被别的东西占着就原地照用，库里的游戏、存档都在；
    /// 已经搬过的按搬库标记落到新位置。旧库不在也没搬过（新装）才用新缺省库。</summary>
    public static string SavedLibrary(string privateRoot, string defaultLibrary)
    {
        try
        {
            var saved = File.ReadAllText(PreferenceFile(privateRoot)).Trim();
            if (saved.Length > 0) return Library(privateRoot, saved);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var legacy = Library(privateRoot, LegacyDefaultLibrary(privateRoot));
        return Built(legacy) ? legacy : defaultLibrary;
    }

    /// <summary>任务日志 + 任务记录（logs\jobs\）：库建好了（有 .s2arm-library.json）是 库\logs，否则退回 私有目录\logs。library 不给就读设置。</summary>
    public static string LogsRoot(string privateRoot, string? library = null)
    {
        var lib = library is null ? SavedLibrary(privateRoot) : Library(privateRoot, library);
        return Path.Combine(!string.IsNullOrEmpty(lib) && Built(lib) ? lib : privateRoot, "logs");
    }

    public static string JobsDir(string privateRoot, string? library = null) => Path.Combine(LogsRoot(privateRoot, library), "jobs");

    private static bool Built(string library) => File.Exists(Path.Combine(library, ".s2arm-library.json"));

    private static bool SamePath(string a, string b)
    {
        try
        {
            return string.Equals(Path.GetFullPath(a).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                                 Path.GetFullPath(b).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar),
                                 OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
        }
        catch (Exception error) when (error is ArgumentException or NotSupportedException or PathTooLongException) { return false; }
    }
}
