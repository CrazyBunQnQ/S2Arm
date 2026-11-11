// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using S2Arm.Launcher.Protocol;

/// <summary>缺省游戏库位置被占时换到旁边（LibraryPaths.Occupied / FreeDefaultLibrary，v0.2.8 热修）：期望按规则手写，临时目录里真建文件夹核。
/// 玩家把 S2Arm 的 zip 解压到了 %USERPROFILE%\S2Arm（缺省库同名同位置），作业层 setup 拒收「已有别的文件」，添加游戏卡死。</summary>
internal static class DefaultLibraryCases
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "s2arm-default-lib-" + Guid.NewGuid().ToString("N"));
        try
        {
            var lib = Path.Combine(root, "S2Arm");
            check(!LibraryPaths.Occupied(lib) && LibraryPaths.FreeDefaultLibrary(lib) == lib, "default library: missing folder is free, kept");
            Directory.CreateDirectory(lib);
            File.WriteAllText(Path.Combine(lib, "desktop.ini"), ""); File.WriteAllText(Path.Combine(lib, "._x"), "");
            check(!LibraryPaths.Occupied(lib), "default library: only OS junk counts as empty");
            File.WriteAllText(Path.Combine(lib, "S2Arm.exe"), "x"); Directory.CreateDirectory(Path.Combine(lib, "Engine"));
            check(LibraryPaths.Occupied(lib), "default library: unzipped launcher files = occupied");
            check(LibraryPaths.FreeDefaultLibrary(lib) == lib + " Library", "default library: occupied → \"S2Arm Library\" next to it");
            Directory.CreateDirectory(lib + " Library"); File.WriteAllText(Path.Combine(lib + " Library", "note.txt"), "x");
            check(LibraryPaths.FreeDefaultLibrary(lib) == lib + " Library 2", "default library: that one occupied too → \"S2Arm Library 2\"");
            File.WriteAllText(Path.Combine(lib, ".s2arm-library.json"), "{}");
            check(!LibraryPaths.Occupied(lib) && LibraryPaths.FreeDefaultLibrary(lib) == lib, "default library: has library marker = an existing library, kept");
            File.Delete(Path.Combine(lib, ".s2arm-library.json")); File.WriteAllText(Path.Combine(lib, "catalog.json"), "{}");
            check(!LibraryPaths.Occupied(lib), "default library: old library without marker (catalog.json) is adoptable, kept");
        }
        finally { try { Directory.Delete(root, true); } catch (IOException) { } }
    }
}
