// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using S2Arm.Launcher.Protocol;

namespace S2Arm.Launcher;

// 批量出包（v0.2.4，照 Mac S2ArmLauncherModel+Batch.swift / BatchExportView.swift）：一次放进多款 → 识别完列成清单，每款勾平台
// （Windows 只出 iPhone，Mac 一栏置灰）→ 一批共用一个存放位置 → 一款接一款地出，每款用出包页同一条单款出包流程（StartBatchPack → RunPackAsync）。
// 清单、状态和规则（下一个出谁、磁盘满暂停、继续 / 重试 / 取消、汇总）都在共用后端 pack/jobs/batch.py（BatchClient），这里只发起作业、画状态。
public sealed partial class EnvironmentWindow
{
    /// <summary>出包前清单里的一行：识别出来的一款，勾没勾 iPhone；不能出的写原因（缺密钥、缺文件），勾选置灰、不进批次</summary>
    private sealed class BatchRow(string id, string name, bool iphone, string? problem)
    {
        public string Id { get; } = id;
        public string Name { get; } = name;
        public bool Iphone { get; set; } = iphone;
        public string? Problem { get; } = problem;
    }

    // 这台电脑能出的平台（Windows 只有 iPhone）
    private static readonly string[] BatchSupported = ["iphone"];
    private List<BatchRow> batchDraft = [];
    private IReadOnlyList<ImportOutcome.Unrecognized> batchUnrecognized = [];
    // 后端批次（batch.py 的结果，出包中途关掉启动器也在）；这批是不是正在一款接一款地出；玩家点了取消、等正在出的那款停下
    private BatchState? batchState;
    private bool batchRunning, batchStopping;
    private string batchError = "";
    /// <summary>控制口 POST /batch 指定的每款平台（AI 操作同 Mac：可以要 Mac，这台出不了的记失败「这台电脑出不了这个平台的包」）；界面清单为 null，按勾选出 iPhone</summary>
    private Dictionary<string, string[]>? batchControlPlatforms;
    private ScrollViewer? batchScroller;

    /// <summary>放进来的几款列成清单（导入完多于 1 款时调）：iPhone 默认跟着出包页的勾选；缺东西的款写原因。正在出一批时只回到清单页</summary>
    private void OpenBatch(IReadOnlyList<string> ids, IReadOnlyList<ImportOutcome.Unrecognized> unrecognized)
    {
        if (batchRunning) { Navigate("batch"); return; }
        batchDraft = ids.Select(id => catalog?.Games.FirstOrDefault(g => string.Equals(g.Id, id, StringComparison.OrdinalIgnoreCase) && !g.Hidden))
            .OfType<LibraryGame>().Select(g =>
            {
                var problem = g.Missing.Count > 0 ? T("library.requirements") + " · " + string.Join(" / ", g.Missing)
                    : g.NeedsKeys && catalog?.KeysReady != true ? T("batch.keys") : null;
                return new BatchRow(g.Id, g.Name, problem is null && exportIos, problem);
            }).ToList();
        batchUnrecognized = unrecognized;
        batchState = null; batchError = "";   // 新清单出包时 create 会盖掉上一批；没出包就退出时重读回来（CloseBatchDraft）
        locationsFor = null;   // 存放位置按第一款重读（剩余空间是当时的）
        Navigate("batch");
    }

    private LibraryGame? BatchGame(string id) => catalog?.Games.FirstOrDefault(g => g.Id == id && !g.Hidden);

    /// <summary>页面（同出包页的 DockPanel：内容滚动、底栏固定）；不是清单页返回 false</summary>
    private bool RenderBatchPage()
    {
        if (page != "batch") return false;
        EnsureTicker();
        var content = new StackPanel { Spacing = 18, Margin = new Thickness(32, 32, 32, 24), MaxWidth = 960 };
        content.Children.Add(Action("export.back", LeaveBatch));
        var title = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), ColumnSpacing = 16 };
        title.Children.Add(Label(T("batch.title"), 30));
        var hint = Muted(T("batch.hint")); hint.VerticalAlignment = VerticalAlignment.Bottom;
        Grid.SetColumn(hint, 1); title.Children.Add(hint);
        content.Children.Add(title);
        if (batchState is { } state)
        {
            content.Children.Add(Label(T("batch.summary").Replace("{0}", state.Summary.Total.ToString())
                .Replace("{1}", state.Summary.Completed.ToString()).Replace("{2}", state.Summary.Failed.ToString()), 16));
            if (state.Paused == "disk_full") { var paused = Muted(T("batch.paused"), 14); paused.Foreground = Brush.Parse("#FFC857"); content.Children.Add(paused); }
        }
        if (batchError.Length > 0) content.Children.Add(new TextBlock { Text = B(batchError), FontSize = 13, Foreground = Brush.Parse("#E55"), TextWrapping = TextWrapping.Wrap });
        if (batchState is null) RenderBatchDraft(content); else RenderBatchProgress(content, batchState);

        var dock = new DockPanel { LastChildFill = true };
        if (PhonePill() is { } pill)
        {
            var top = new Border { Padding = new Thickness(32, 14, 32, 0), Child = pill, HorizontalAlignment = HorizontalAlignment.Right };
            DockPanel.SetDock(top, Dock.Top); dock.Children.Add(top);
        }
        var bar = BatchBottomBar();
        DockPanel.SetDock(bar, Dock.Bottom); dock.Children.Add(bar);
        var offset = batchScroller?.Offset ?? default;
        batchScroller = new ScrollViewer { Content = content, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        if (offset != default) { var scroller = batchScroller; Dispatcher.UIThread.Post(() => scroller.Offset = offset, DispatcherPriority.Loaded); }
        dock.Children.Add(batchScroller);
        Content = dock;
        return true;
    }

    // ---- 出包前 ----

    private void RenderBatchDraft(StackPanel content)
    {
        if (batchDraft.FirstOrDefault() is { } first && BatchGame(first.Id) is { } firstGame) content.Children.Add(LocationRow(firstGame, "batch.location"));
        var ok = batchDraft.Where(r => r.Problem is null).ToList();
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto"), ColumnSpacing = 10, Margin = new Thickness(14, 6, 14, 0) };
        head.Children.Add(Muted(T("batch.game")));
        var allIphone = Action("batch.all", () =>
        {
            var all = ok.All(r => r.Iphone);
            foreach (var r in ok) r.Iphone = !all;
            Render();
        }, ok.Count > 0 && !batchRunning);
        allIphone.Content = T("batch.all") + " iPhone";
        AutomationId(allIphone, "batch.all.iphone");
        Grid.SetColumn(allIphone, 1); head.Children.Add(allIphone);
        var allMac = Action("batch.all", () => { }, false);   // Windows 出不了 Mac 版：置灰
        allMac.Content = T("batch.all") + " Mac";
        AutomationId(allMac, "batch.all.mac");
        Grid.SetColumn(allMac, 2); head.Children.Add(allMac);
        content.Children.Add(head);
        // Mac 一栏置灰的原因写出来（同单款出包页 Mac 卡片「仅 Mac 可出」）
        var macOnly = Muted(T("batch.mac_only")); macOnly.HorizontalAlignment = HorizontalAlignment.Right; macOnly.Margin = new Thickness(14, 0, 14, 0);
        AutomationId(macOnly, "batch.mac_only");
        content.Children.Add(macOnly);
        foreach (var row in batchDraft)
        {
            var line = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto"), ColumnSpacing = 14 };
            line.Children.Add(BatchCover(row.Id));
            var name = Label(row.Name, 15); name.VerticalAlignment = VerticalAlignment.Center; name.TextWrapping = TextWrapping.NoWrap; name.TextTrimming = TextTrimming.CharacterEllipsis;
            Grid.SetColumn(name, 1); line.Children.Add(name);
            if (row.Problem is { } problem)
            {
                var why = Muted(problem); why.Foreground = Brush.Parse("#FFC857"); why.VerticalAlignment = VerticalAlignment.Center; why.MaxWidth = 360;
                Grid.SetColumn(why, 2); Grid.SetColumnSpan(why, 2); line.Children.Add(why);
            }
            else
            {
                var iphone = new CheckBox { Content = "iPhone", IsChecked = row.Iphone, IsEnabled = !batchRunning, MinWidth = 96, VerticalAlignment = VerticalAlignment.Center };
                AutomationId(iphone, "batch.iphone." + row.Id);
                iphone.IsCheckedChanged += (_, _) => { row.Iphone = iphone.IsChecked == true; Render(); };
                Grid.SetColumn(iphone, 2); line.Children.Add(iphone);
                var mac = new CheckBox { Content = "Mac", IsChecked = false, IsEnabled = false, MinWidth = 70, VerticalAlignment = VerticalAlignment.Center };
                AutomationId(mac, "batch.mac." + row.Id);
                Grid.SetColumn(mac, 3); line.Children.Add(mac);
            }
            content.Children.Add(Card(line, 12));
        }
        foreach (var source in batchUnrecognized)
        {
            var line = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 14 };
            line.Children.Add(new Border { Width = 64, Height = 36, CornerRadius = new CornerRadius(6), Background = Brush.Parse("#26262A"),
                Child = new TextBlock { Text = "?", FontSize = 18, Foreground = Brush.Parse("#8C8A85"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
            var name = Label(source.FileName, 15); name.VerticalAlignment = VerticalAlignment.Center; name.TextWrapping = TextWrapping.NoWrap; name.TextTrimming = TextTrimming.CharacterEllipsis;
            Grid.SetColumn(name, 1); line.Children.Add(name);
            var why = Muted(T("batch.unrecognized").Replace("{0}", B(source.Reason))); why.Foreground = Brush.Parse("#E05C5C"); why.VerticalAlignment = VerticalAlignment.Center; why.MaxWidth = 360;
            Grid.SetColumn(why, 2); line.Children.Add(why);
            content.Children.Add(Card(line, 12));
        }
    }

    // ---- 出包中 / 出完 ----

    private void RenderBatchProgress(StackPanel content, BatchState state)
    {
        foreach (var item in state.Items)
        {
            var line = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), ColumnSpacing = 14 };
            line.Children.Add(BatchCover(item.GameId));
            var text = new StackPanel { Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
            var name = Label(item.Name, 15); name.TextWrapping = TextWrapping.NoWrap; name.TextTrimming = TextTrimming.CharacterEllipsis;
            text.Children.Add(name);
            foreach (var p in item.Platforms) text.Children.Add(BatchPlatformLine(item.GameId, p, item.Result(p)));
            Grid.SetColumn(text, 1); line.Children.Add(text);
            var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
            if (item.Any("failed"))
            {
                if (item.Platforms.Any(p => item.Result(p).Code == "disk_full")) actions.Children.Add(Action("batch.free", OpenStorage, !PackBusy));   // 同出包失败页「释放空间」
                var retry = Action("batch.retry", () => _ = BatchCommandAsync("retry", ["--game", item.GameId]), !PackBusy);
                AutomationId(retry, "batch.retry." + item.GameId);
                actions.Children.Add(retry);
            }
            if (item.Platforms.Contains("iphone") && item.Result("iphone").State == "completed" && BatchGame(item.GameId) is { } game)
                actions.Children.Add(Action("batch.install", () => { selectedGame = game.Id; readinessFor = null; Navigate("install"); }, !PackBusy));
            Grid.SetColumn(actions, 2); line.Children.Add(actions);
            content.Children.Add(Card(line, 12));
        }
    }

    private Control BatchPlatformLine(string gameId, string platform, BatchResult r)
    {
        var running = r.State == "running" && packRunning && packGame?.Id == gameId;
        var text = (platform == "iphone" ? "iPhone" : "Mac") + " · " + T("batch.state." + r.State);
        if (running && packTrack.Plan.FirstOrDefault(m => m.Id == packTrack.Current) is { } stage) text += " · " + B(stage.Label);
        if (r.State == "pending" && r.Interrupted) text += " · " + T("batch.interrupted");
        if (r.State == "failed" && r.Message.Length > 0) text += "：" + B(r.Message);
        var (mark, color) = r.State switch
        {
            "completed" => ("✓", "#52C87A"), "failed" => ("✕", "#E05C5C"), "running" => ("●", "#FFC857"), "cancelled" => ("–", "#8C8A85"), _ => ("○", "#8C8A85")
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        row.Children.Add(new TextBlock { Text = mark, FontSize = 13, Foreground = Brush.Parse(color) });
        var label = new TextBlock { Text = text, FontSize = 12.5, TextWrapping = TextWrapping.Wrap, MaxWidth = 560,
            Foreground = r.State == "failed" ? Brush.Parse("#E05C5C") : Brush.Parse("#A8A6A1") };
        row.Children.Add(label);
        return row;
    }

    private Control BatchCover(string gameId)
    {
        Control cover = BatchGame(gameId) is { } game ? GameCover(game, 48) : new Border { Width = 48, Height = 48, Background = Brush.Parse("#26262A") };
        return new Border { Width = 48, Height = 48, CornerRadius = new CornerRadius(6), ClipToBounds = true, Child = cover, VerticalAlignment = VerticalAlignment.Center };
    }

    // ---- 底栏 ----

    private Control BatchBottomBar()
    {
        var inner = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,Auto,*,Auto"), ColumnSpacing = 12 };
        void Put(Control c, int column) { Grid.SetColumn(c, column); inner.Children.Add(c); }
        if (batchState is null)
        {
            var count = batchDraft.Count(r => r.Problem is null && r.Iphone);
            var start = Primary(T("batch.start").Replace("{0}", count.ToString()), () => _ = StartBatchAsync(), count > 0 && !PackBusy && !libraryBusy && !batchRunning);
            AutomationId(start, "batch.start");
            Put(start, 3);
        }
        else if (batchRunning)
        {
            var cancel = Action("batch.cancel", StopBatch, !batchStopping);
            AutomationId(cancel, "batch.cancel");
            Put(cancel, 3);
        }
        else
        {
            // 暂停着（磁盘满）只给「重试」：它解除暂停、重出失败的那个平台；「继续」不解除暂停（审计 P1-164，同 Mac）
            if (batchState.Summary.Failed > 0 || batchState.Paused is not null) Put(Action("batch.retry_failed", () => _ = BatchCommandAsync("retry"), !PackBusy), 0);
            if (batchState.Paused is null && (batchState.Summary.Cancelled > 0 || batchState.Summary.Pending > 0))
            {
                var resume = Action("batch.resume", () => _ = BatchCommandAsync("resume"), !PackBusy);
                AutomationId(resume, "batch.resume");
                Put(resume, 1);
            }
            var close = Action("batch.close", () => _ = CloseBatchAsync());
            AutomationId(close, "batch.close");
            Put(close, 3);
        }
        return new Border { BorderBrush = Brush.Parse("#12FFFFFF"), BorderThickness = new Thickness(0, 1, 0, 0), Padding = new Thickness(32, 14), Child = inner };
    }

    /// <summary>顶栏胶囊（同 Mac batchPill）：正在出一批，或上次还有没出完的；点了回清单页</summary>
    private Button? BatchPill()
    {
        if (page == "batch" || !(batchRunning || (batchState?.Unfinished ?? 0) > 0)) return null;
        var pill = new Button
        {
            Content = batchRunning ? T("batch.title") : T("batch.unfinished").Replace("{0}", batchState!.Unfinished.ToString()),
            Padding = new Thickness(14, 8), CornerRadius = new CornerRadius(16), Background = Brush.Parse("#29292D"), Foreground = Ink
        };
        AutomationId(pill, "top.batch");
        pill.Click += (_, _) => { if (!libraryBusy) importDialog = null; Navigate("batch"); };
        return pill;
    }

    // ---- 流程 ----

    /// <summary>后端跑一条批次命令，结果换上 batchState；失败写在页面上，返回 false</summary>
    private async Task<bool> BatchRunAsync(string command, IEnumerable<string>? arguments = null)
    {
        var outcome = await BatchClient.RunAsync(paths, privateRoot, command, arguments);
        if (outcome.Error is { } error) { batchError = error; return false; }
        batchError = "";
        batchState = outcome.State;
        return true;
    }

    private Task<bool> BatchMarkAsync(string gameId, string platform, string state, string? message = null, string? code = null, string? output = null) =>
        BatchRunAsync("mark", BatchClient.MarkArguments(gameId, platform, state, message, code, output));

    /// <summary>点「出包（N 款）」：后端建批次（一批共用出包页选的存放位置），开始出第一款</summary>
    /// <param name="installAfter">「更新并重新出这几款」排的批：整批出完逐款装到 iPhone（InstallModePage.cs BatchFinishedAsync）</param>
    private async Task StartBatchAsync(bool installAfter = false)
    {
        if (batchRunning || PackBusy) return;
        batchInstallAfter = installAfter;   // 每批自己定：界面 / 控制口建的批不接着装
        var control = batchControlPlatforms; batchControlPlatforms = null;
        var entries = control is null
            ? batchDraft.Where(r => r.Problem is null && r.Iphone).Select(r => new BatchClient.Entry(r.Id, r.Name, ["iphone"])).ToList()
            : batchDraft.Where(r => r.Problem is null && control.ContainsKey(r.Id)).Select(r => new BatchClient.Entry(r.Id, r.Name, control[r.Id])).ToList();
        if (entries.Count == 0) return;
        var location = locationsFor == batchDraft[0].Id && exportLocation.Length > 0 ? exportLocation : null;
        if (!await BatchRunAsync("create", ["--json", BatchClient.CreateJson(entries, location)]) || batchState is null) { Render(); return; }
        batchDraft = []; batchUnrecognized = [];
        batchRunning = true;
        await AdvanceBatchAsync();
    }

    /// <summary>出下一款：后端说出谁就出谁；没有下一个 = 出完 / 暂停 / 取消了。正在出的那款结束时（BatchPackEndedAsync）再走这里</summary>
    private async Task AdvanceBatchAsync()
    {
        while (batchRunning)
        {
            if (batchStopping) { await StopBatchAsync(); return; }
            if (packRunning) return;
            if (libraryOperation is { IsCompleted: false } reload) { await reload; continue; }   // 上一款出好后重读游戏库：读完再出下一款
            if (batchState?.Step(BatchSupported) is not { } run) { batchRunning = false; Render(); _ = BatchFinishedAsync(); return; }
            var gid = run.Item.GameId;
            var marked = true;
            foreach (var p in run.Unsupported) marked &= await BatchMarkAsync(gid, p, "failed", "这台电脑出不了这个平台的包");
            var game = BatchGame(gid);
            if (marked && run.Runnable.Count > 0 && (game is null || connectedFolder is null || libraryId is null))
                foreach (var p in run.Runnable) marked &= await BatchMarkAsync(gid, p, "failed", "游戏库里已经没有这款游戏");   // 玩家删了：记失败、接着下一款
            else if (marked && run.Runnable.Count > 0)
            {
                if (!await BatchMarkAsync(gid, run.Runnable[0], "running")) { batchRunning = false; Render(); return; }
                if (batchStopping) { await StopBatchAsync(); return; }
                StartBatchPack(game!);
                return;
            }
            if (!marked) { batchRunning = false; Render(); return; }   // 记不进批次就停下（不然下一个永远是这款）
        }
    }

    /// <summary>单款出包流程出这款的 iPhone 包：存放位置用这批共用的那个；留在清单页看整批进度（单款进度页可从游戏库胶囊打开）</summary>
    private void StartBatchPack(LibraryGame game)
    {
        packLocation = batchState?.Location;
        ResetPack(game);
        sessionHidden = true;
        Render();
        packOperation = RunPackAsync(game, false);
    }

    /// <summary>这款出完（RunPackAsync 收尾调）：取消 = 整批停（已出好的保留）；否则记结果、接着出下一款</summary>
    private async Task BatchPackEndedAsync(LibraryGame game)
    {
        if (packState == "cancelled") { await StopBatchAsync(); return; }
        var why = packSummary.Length > 0 ? packSummary : packDiagnostics.Split('\n').FirstOrDefault(l => l.Trim().Length > 0) ?? "";
        var marked = packState == "succeeded"
            ? await BatchMarkAsync(game.Id, "iphone", "completed", output: packIpa)
            : await BatchMarkAsync(game.Id, "iphone", "failed", why, packFailureCode);
        if (!marked) { batchRunning = false; Render(); return; }
        Render();
        await AdvanceBatchAsync();
    }

    /// <summary>底栏「取消」：正在出的那款停下后整批取消；两款之间点的，下一款不再开始</summary>
    private void StopBatch()
    {
        batchStopping = true;
        packCancellation?.Cancel();
        Render();
    }

    private async Task StopBatchAsync()
    {
        await BatchRunAsync("cancel");
        batchRunning = false; batchStopping = false;
        Render();
    }

    /// <summary>继续出剩下的（取消后 / 关掉启动器再打开后）、重试失败的（给游戏号只重试这一款；磁盘满那款释放空间后也走这里）</summary>
    private async Task BatchCommandAsync(string command, IEnumerable<string>? arguments = null)
    {
        if (packRunning) return;
        if (!await BatchRunAsync(command, arguments)) { Render(); return; }
        if (batchRunning) { Render(); return; }   // 正在出的这批会自己取到重新排队的
        batchRunning = true;
        await AdvanceBatchAsync();
    }

    /// <summary>返回：出包中 / 出完都只是离开清单页（批次留着，顶栏胶囊能回来）；出包前的清单不要了，重读上一批（可能还有没出完的）</summary>
    private void LeaveBatch()
    {
        if (batchState is null && !batchRunning) { batchDraft = []; batchUnrecognized = []; _ = ReloadBatchAsync(); }
        Navigate("library");
    }

    /// <summary>「完成」：这批的结果看完了，清掉批次（不影响已出好的包）</summary>
    private async Task CloseBatchAsync()
    {
        if (batchRunning) { Navigate("library"); return; }
        if (await BatchRunAsync("clear")) batchState = null;
        batchDraft = []; batchUnrecognized = [];
        Navigate("library");
    }

    private async Task ReloadBatchAsync()
    {
        if (await BatchRunAsync("show")) Render();
    }

    /// <summary>启动器刚打开：上次没出完的批次读回来（正在出的那款记成被打断、重新排队），顶栏胶囊提示继续。没有批次时后端报错，不显示</summary>
    private async Task RecoverBatchAsync()
    {
        var outcome = await BatchClient.RunAsync(paths, privateRoot, "recover");
        if (outcome.State is null) return;
        batchState = outcome.State;
        Render();
    }
}
