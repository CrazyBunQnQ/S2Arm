// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Avalonia Headless snapshot runner: 欢迎页 + 出包页各状态 → PNG。
// 只用于截图验收，不进发布包。VisualTests 已并入本工程。
using System.Reflection;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Headless;
using Avalonia.Skia;
using Avalonia.Themes.Fluent;
using Avalonia.Styling;
using Avalonia.Threading;
using S2Arm.Launcher;
using S2Arm.Launcher.Protocol;

var outDir = args.Length > 0 ? args[0] : Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".s2arm/run/s2arm/cmdr/evidence/launcher/win");
Directory.CreateDirectory(outDir);

AppBuilder.Configure<SnapshotApp>()
    .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
    .With(new SkiaOptions())
    .UseSkia()
    .SetupWithoutStarting();

// ── 统一夹具游戏（不用固定游戏名字符串以外的占位）────────────────────
static LibraryGame MockGame(bool keysReady = true) => new(
    "0100B040192FA000", "Fire Emblem: Fortune's Weave", "v1.0.1",
    "", false, [], keysReady, keysReady ? DateTimeOffset.UtcNow.ToUnixTimeSeconds() - 86400.0 : null,
    true, [new("base", "0100B040192FA000", 917504, "v1.0.1", 31_000_000_000L, "")]);

static LibraryCatalog MockCatalog(bool keysReady = true)
{
    var game = MockGame(keysReady);
    return new LibraryCatalog([game], keysReady, keysReady ? "密钥已就绪" : "");
}

// ── 单窗口复用：欢迎页场景 ──────────────────────────────────────────────
static EnvironmentReport FakeReport(bool ready) => EnvironmentReport.Parse(
    ready
    ? "{\"schema\":1,\"ready\":true,\"checks\":[{\"id\":\"host\",\"name\":\"Windows 64位\",\"status\":\"ok\",\"ok\":true,\"message\":\"就绪\",\"code\":null,\"action\":null}]}"
    : "{\"schema\":1,\"ready\":false,\"checks\":[{\"id\":\"python\",\"name\":\"Python\",\"status\":\"missing\",\"ok\":false,\"message\":\"需要 Python 3.11\",\"code\":\"python_missing\",\"action\":\"安装 Python\",\"url\":\"https://www.python.org\"}]}",
    ready ? 0 : 1);

var welcome = new EnvironmentWindow();
welcome.Width = 1280; welcome.Height = 900;
welcome.Show();
Dispatcher.UIThread.RunJobs();

void SaveW(string name, Action<EnvironmentWindow> setup)
{
    setup(welcome);
    SetW(welcome, "page", "library");
    CallW(welcome, "Render");
    Dispatcher.UIThread.RunJobs();
    var bmp = welcome.CaptureRenderedFrame();
    if (bmp is not null) { using var s = File.OpenWrite(Path.Combine(outDir, name + ".png")); bmp.Save(s); Console.WriteLine("  saved: " + name); }
    else Console.WriteLine("  WARN no frame: " + name);
}

SaveW("01_welcome_empty", w => { SetW(w, "report", null); SetW(w, "busy", false); SetW(w, "catalog", null); });
SaveW("02_welcome_env_checking", w => { SetW(w, "busy", true); SetW(w, "summaryKey", "environment.checking"); });
SaveW("03_welcome_env_ready_no_games", w => { SetW(w, "busy", false); SetW(w, "report", FakeReport(true)); SetW(w, "catalog", null); });
SaveW("04_welcome_games_chosen", w => {
    SetW(w, "report", FakeReport(true));
    SetW(w, "pendingImportPaths", new[] { "/tmp/fe_fortune.nsz" });
});
SaveW("05_welcome_oneclick", w => {
    SetW(w, "report", FakeReport(true));
    SetW(w, "welcomeGameId", MockGame().Id);
    SetW(w, "catalog", MockCatalog());
    SetW(w, "connectedFolder", "/tmp/s2arm-library");
    SetW(w, "libraryId", "mock-lib-id");
});
// 顶部公告条（v0.2.3）：内容取仓库里那份 docs/site/notice.json（启动器实际读的同一份），中 / 英各一张
var noticeDir = new DirectoryInfo(AppContext.BaseDirectory);
while (noticeDir is not null && !File.Exists(Path.Combine(noticeDir.FullName, "docs/site/notice.json"))) noticeDir = noticeDir.Parent;
var noticeJson = File.ReadAllText(Path.Combine(noticeDir?.FullName ?? throw new FileNotFoundException("docs/site/notice.json"), "docs/site/notice.json"));
SaveW("06_notice_zh", w => { SetW(w, "notice", (JsonElement?)JsonDocument.Parse(noticeJson).RootElement); SetW(w, "language", "zh-Hans"); });
SaveW("07_notice_en", w => { SetW(w, "language", "en"); });
SetW(welcome, "notice", null); SetW(welcome, "language", "zh-Hans");

welcome.Close();
Dispatcher.UIThread.RunJobs();

// ── 单窗口复用：出包页场景 ──────────────────────────────────────────────
var pack = new EnvironmentWindow();
pack.Width = 1280; pack.Height = 900;
pack.Show();
Dispatcher.UIThread.RunJobs();

void SaveP(string name, Action setup)
{
    setup();
    Dispatcher.UIThread.RunJobs();
    var bmp = pack.CaptureRenderedFrame();
    if (bmp is not null) { using var s = File.OpenWrite(Path.Combine(outDir, name + ".png")); bmp.Save(s); Console.WriteLine("  saved: " + name); }
    else Console.WriteLine("  WARN no frame: " + name);
}

InjectMockGame(pack, ready: false);
SaveP("win_01_export_select", () => Navigate(pack, "build"));

InjectMockProgress(pack, fraction: 0.05, stepIndex: 0, running: true);
SaveP("win_02_export_progress_running", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 0.60, stepIndex: 1, running: true);
SaveP("win_03_export_progress_mid", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 1.0, stepIndex: -1, running: false, state: "succeeded",
    ipaBytes: 1_200_000_000L, packBytes: 3_400_000_000L, elapsed: 247);
SaveP("win_04_export_success", () => Navigate(pack, "progress"));

InjectMockProgress(pack, fraction: 0.12, stepIndex: 0, running: false, state: "failed",
    summary: "磁盘空间不足，无法继续出包。");
SaveP("win_05_export_failed", () => Navigate(pack, "progress"));

InjectMockGame(pack, ready: true);
SaveP("win_06_library_iphone_ready", () => Navigate(pack, "library"));

InjectMockGame(pack, ready: false);
InjectMockProgress(pack, fraction: 0.35, stepIndex: 1, running: true);
SetW(pack, "sessionHidden", true);
SaveP("win_07_library_capsule", () => Navigate(pack, "library"));

// 「打开游戏文件夹」找不到文件夹：按钮下方说明原因，不打开空的资源管理器
InjectMockProgress(pack, fraction: 1.0, stepIndex: -1, running: false, state: "succeeded",
    ipaBytes: 1_200_000_000L, packBytes: 3_400_000_000L, elapsed: 247);
SetW(pack, "revealProblem", "找不到这款游戏的文件夹：所在的磁盘可能没接上，或文件夹被移走了。");
SaveP("win_08_export_success_folder_missing", () => Navigate(pack, "progress"));
SetW(pack, "revealProblem", "");

// 封面兜底：游戏包没带图标时用商店横图（banner.jpg），在方形卡片里铺满裁切；没有横图才是 S2Arm 标志
var bannerSrc = Environment.GetEnvironmentVariable("S2A_SNAPSHOT_BANNER") ?? "";
if (File.Exists(bannerSrc))
{
    var folder = Path.Combine(Path.GetTempPath(), "s2a-snap-" + Guid.NewGuid().ToString("N"), "games", "Home Sheep Home");
    Directory.CreateDirectory(folder);
    File.Copy(bannerSrc, Path.Combine(folder, "banner.jpg"));
    var withBanner = MockGame(true) with { Id = "010076000F6B8000", Name = "送小羊回家: 农场末日", Cover = Path.Combine(folder, "banner.jpg"),
        Location = new GameLocation("ok", Path.GetDirectoryName(Path.GetDirectoryName(folder))!, folder, "", true) };
    var noCover = MockGame(true) with { Id = "010024B00F9D0000", Name = "泰拉瑞亚", Cover = "" };
    SetW(pack, "catalog", new LibraryCatalog([withBanner, noCover, MockGame(true)], true, "密钥已就绪"));
    SetW(pack, "connectedFolder", Path.GetDirectoryName(Path.GetDirectoryName(folder))!);
    SaveP("win_09_library_banner_fallback", () => Navigate(pack, "library"));
}

// 出包页「签名方式」（v0.2.3 开发者证书签名，DevCertPage.cs）：Apple ID 模式；证书模式（团队、到期、包名、设备核对、缺权限黄字）。
//   devCert 直接放作业结果（不跑 devCert 作业）；窗口加高，免得签名一栏落在首屏外
pack.Height = 1300;
InjectMockGame(pack, ready: true);
SetW(pack, "exportIos", true);
SetW(pack, "devCert", JsonDocument.Parse("{\"imported\":false,\"active\":false,\"cert\":null}").RootElement.Clone());
SaveP("win_10_export_sign_appleid", () => Navigate(pack, "build"));
SetW(pack, "devCert", JsonDocument.Parse("""
{"imported":true,"active":true,"cert":{"blocking":[],"warnings":[
 {"id":"devcert_entitlements","text":"描述文件没开「扩展寻址」权限：小游戏照常，大游戏可能内存不够闪退。可在开发者网站给这个 App ID 打开后重新下载描述文件。"},
 {"id":"devcert_expiring","text":"证书将于 2026-11-30 到期（还剩 20 天）：到期后 App 打不开，记得提前换新。"}],
 "teamName":"S2Arm Test Co","teamId":"ABCDE12345","certExpires":"2026-11-30","profileExpires":"2027-05-29",
 "bundleId":"io.s2arm.games.abcde12345","wildcard":true,"allDevices":false,"deviceCount":1,"deviceListed":null}}
""").RootElement.Clone());
SaveP("win_11_export_sign_devcert", () => Navigate(pack, "build"));

// 批量出包清单页（BatchPage.cs）：出包前（勾选、Mac 置灰、缺密钥、缺文件、认不出）；出完有成有败（磁盘满暂停、释放空间、重试、装到 iPhone、上次被打断）。
//   清单由 OpenBatch 按游戏库生成；出完的状态直接放 batch.py 形状的结果（不跑 batch.py）
pack.Height = 900;
static LibraryGame BatchMock(string id, string name, string format, IReadOnlyList<string>? missing = null) => MockGame(true) with
{
    Id = id, Name = name, Missing = missing ?? [], Packages = [new("base", id, 0, "v1.0.0", 4_000_000_000L, format)]
};
SetW(pack, "catalog", new LibraryCatalog([
    BatchMock("0100AAAA00000000", "星海远征", "extract"), BatchMock("0100BBBB00000000", "Pixel Garden", "extract"),
    BatchMock("0100CCCC00000000", "雪原小镇", "nsp"), BatchMock("0100DDDD00000000", "Clockwork Isles", "extract", ["本体"]),
    BatchMock("0100EEEE00000000", "Harbor Lights", "extract")], false, ""));
SetW(pack, "connectedFolder", "/tmp/s2arm-library");
SetW(pack, "libraryId", "mock-lib-id");
SetW(pack, "batchState", null);
CallW(pack, "OpenBatch", new List<string> { "0100AAAA00000000", "0100BBBB00000000", "0100CCCC00000000", "0100DDDD00000000", "0100EEEE00000000" },
    new List<ImportOutcome.Unrecognized> { new("readme.txt", "不是 NSP、XCI 或解包好的游戏文件夹。", "input_type_unsupported") });
SaveP("win_12_batch_draft", () => { });
SetW(pack, "batchState", BatchClient.Read(JsonDocument.Parse("""
{"batch":{"schema":1,"location":null,"paused":"disk_full","items":[
 {"gameId":"0100AAAA00000000","name":"星海远征","platforms":["iphone"],"results":{"iphone":{"state":"completed","output":"/tmp/a.ipa"}}},
 {"gameId":"0100BBBB00000000","name":"Pixel Garden","platforms":["iphone"],"results":{"iphone":{"state":"failed","message":"游戏文件读不出来，可能已损坏。","code":"content_unreadable"}}},
 {"gameId":"0100EEEE00000000","name":"Harbor Lights","platforms":["iphone"],"results":{"iphone":{"state":"failed","message":"磁盘空间不足，无法继续出包。","code":"disk_full"}}},
 {"gameId":"0100CCCC00000000","name":"雪原小镇","platforms":["iphone"],"results":{"iphone":{"state":"pending","interrupted":true}}}]},
 "next":null,"summary":{"total":4,"completed":1,"failed":2,"cancelled":0,"pending":1,"done":false}}
""").RootElement.Clone()).State);
SaveP("win_13_batch_done_mixed", () => Navigate(pack, "batch"));

pack.Close();
Dispatcher.UIThread.RunJobs();
Console.WriteLine("done.");

// ── 辅助函数 ─────────────────────────────────────────────────────────────
static void SetW(object obj, string field, object? value)
{
    var fi = obj.GetType().GetField(field, BindingFlags.NonPublic | BindingFlags.Instance);
    if (fi is not null) { fi.SetValue(obj, value); return; }
    var pi = obj.GetType().GetProperty(field, BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Public);
    if (pi is not null) { pi.SetValue(obj, value); return; }
    throw new MissingMemberException($"Field/property not found: {field}");
}

static object? GetW(object obj, string field)
    => obj.GetType().GetField(field, BindingFlags.NonPublic | BindingFlags.Instance)?.GetValue(obj);

static void CallW(object target, string method, params object?[] methodArgs)
{
    var m = target.GetType().GetMethod(method, BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new InvalidOperationException("missing method " + method);
    m.Invoke(target, methodArgs);
}

static void Navigate(EnvironmentWindow w, string pg)
    => w.GetType().GetMethod("Navigate", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(w, [pg]);

static void InjectMockGame(EnvironmentWindow w, bool ready)
{
    var game = MockGame(ready);
    var catalog = MockCatalog(ready);
    SetW(w, "catalog", catalog);
    SetW(w, "selectedGame", game.Id);
    SetW(w, "connectedFolder", "/tmp/s2arm-library");
    SetW(w, "libraryId", "mock-lib-id");
    SetW(w, "librarySummary", "library.completed");
    if (!ready) return;
    var finishedPacks = (Dictionary<string, (string state, string ipa, long? ipaBytes, long? packBytes, double seconds)>)
        GetW(w, "finishedPacks")!;
    finishedPacks[game.Id] = ("succeeded", "/tmp/game.ipa", 1_200_000_000L, 3_400_000_000L, 247);
}

static void InjectMockProgress(EnvironmentWindow w, double fraction, int stepIndex, bool running,
    string state = "running", string? summary = null, long? ipaBytes = null, long? packBytes = null,
    double elapsed = 0)
{
    var game = MockGame(false);
    SetW(w, "packGame", game);
    SetW(w, "packState", state);
    SetW(w, "packRunning", running);
    SetW(w, "packStarted", elapsed > 0 ? DateTime.UtcNow.AddSeconds(-elapsed) : DateTime.UtcNow.AddSeconds(-30));
    if (!running && elapsed > 0) SetW(w, "packSeconds", (object?)elapsed);
    if (summary is not null) SetW(w, "packSummary", summary);
    if (ipaBytes is not null) SetW(w, "packIpaBytes", (object?)ipaBytes);
    if (packBytes is not null) SetW(w, "packPackBytes", (object?)packBytes);
    if (ipaBytes is not null) SetW(w, "packIpa", "/tmp/game.ipa");

    var track = new MilestoneTrack();
    var planJson = """
[{"id":"unpack","label":"解包","purpose":"读出游戏程序和资源，合并更新与 DLC。","expected":120,"source":"history","samples":3,"reuse":false},
 {"id":"prepare","label":"准备资源","purpose":"整理 DLC 和宽屏界面布局，同时在后台把游戏代码改写成苹果设备能直接运行的形式。","expected":80,"source":"history","samples":3,"reuse":false},
 {"id":"ipa","label":"编 App 与签名","purpose":"编出 iPhone / iPad 版程序、签名，并打好要推进手机的资源包。","expected":40,"source":"history","samples":3,"reuse":false}]
""";
    var planEvent = JsonDocument.Parse($$"""{"type":"plan","milestone":null,"milestones":{{planJson}}}""");
    track.Ingest(planEvent.RootElement, 0);
    // 用真实 Clock 基准注入步骤开始时间，避免 StepElapsed 溢出
    double realClock = DateTime.UtcNow.Subtract(DateTime.UnixEpoch).TotalSeconds;
    if (stepIndex >= 0)
    {
        var ids = new[] { "unpack", "prepare", "ipa" };
        var milestoneEvent = JsonDocument.Parse($$"""{"type":"stage","milestone":"{{ids[stepIndex]}}","message":""}""");
        track.Ingest(milestoneEvent.RootElement, realClock - 30);
    }
    if (!running && state == "succeeded") track.Finish(elapsed > 0 ? elapsed : 30);
    SetW(w, "packTrack", track);

    var spaceJson = """{"volumes":[{"role":"library","needBytes":79890000000,"freeBytes":153420000000,"basis":"rule","enough":true}]}""";
    using var spaceDoc = JsonDocument.Parse(spaceJson);
    SetW(w, "packSpace", PackSpace.TryParse(spaceDoc.RootElement));
}

public sealed class SnapshotApp : Application
{
    public override void Initialize()
    {
        RequestedThemeVariant = ThemeVariant.Dark;
        Styles.Add(new FluentTheme());
    }
}
