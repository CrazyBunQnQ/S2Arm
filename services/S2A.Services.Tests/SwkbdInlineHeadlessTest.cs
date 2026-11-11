// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using Ryujinx.HLE.HOS.Applets.SoftwareKeyboard;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace S2A.Services.Tests
{
    // 内嵌软键盘无界面自动应答（11-04 GRID Autosport 卡「输入名字」）：游戏建内嵌键盘后第一次 Calc 就是 Initialize | Appear | SetCursorPos（0x15）、
    //   并要了 UseChangedStringV2。真机上玩家的输入只会在键盘出现、游戏看到 Shown 之后的若干帧才到，不会和 Appear 的应答同一批。
    //   期望按 swkbd 内嵌协议手写（应答 = 状态 u32 + 类型 u32；0 FinishedInitialize、1 Default、2 ChangedString、5 DecidedEnter；状态 0 未初始化、
    //   1 Initialized、3 Shown、4 Disappearing）：① Calc 当批游戏一口气取到的只有 0 / 2 / 1，没有 DecidedEnter；② 半秒后游戏再取：
    //   ChangedString(Shown) → DecidedEnter → Default(Disappearing) → Default(Initialized)；名字按 Appear 给的最长 4 截成「Play」（宿主规则交「Player」）。
    //   先报改字（11-07 卡顿线）：真键盘逐字回 ChangedString，GRID 从改字回包取文本；只发 DecidedEnter 时真机名字栏停在默认 / 空（同包对照）。
    //   ③ 长度补齐不引入文本外字符：数字框最短 2 交「0」→「00」（Ryujinx 前台键盘的规则是空格连接，得「0 0」）。
    //   修前：① 就收到 0 2 1 5 1 1（DecidedEnter 和 Appear 同一批），GRID 丢掉这次确定、名字栏一直空
    public static unsafe partial class Program
    {
        [SvcCase]
        private static void SwkbdInlineHeadless()
        {
            TextInput.BeginHook = (_, _) => 0;   // 平台没登记系统输入框（同安卓 / 开发宿主 / 无人值守）：走自动应答（单测进程没有原生库）
            try { SwkbdInlineHeadlessBody(); } finally { TextInput.BeginHook = null; }
        }

        private static void SwkbdInlineHeadlessBody()
        {
            Encoding.ASCII.GetBytes("sm:\0").CopyTo(new Span<byte>(_game, 4));
            uint sm = (uint)Svc(0x1F, 0, 0, (ulong)_game).Item2[1];
            Reply goe = Call(sm, Msg.Cmif(1, Name8("appletOE")));
            uint oe = goe.Move.Count == 1 ? goe.Move[0] : 0;
            Msg cd = Msg.Cmif(0); cd.Type = 5;
            int root = *(int*)Call(oe, cd).Data;
            byte* buf = FakeNative.Alloc(0x2000);

            Reply Domain(Msg m, int obj)
            {
                m.DomainObject = obj;
                m.Write(_tlr);
                Svc(0x21, FakeNative.CookieOf(oe));
                return Reply.ParseDomain(_tlr);
            }

            int OutObj(Reply r) => *(int*)r.Data;
            string first, later, negWords = "未跑";

            try
            {
                Msg opx = Msg.Cmif(0, Bytes(0UL)); opx.SendPid = true;
                int proxy = OutObj(Domain(opx, root));                                  // OpenApplicationProxy
                int creator = OutObj(Domain(Msg.Cmif(11), proxy));                      // GetLibraryAppletCreator
                int accessor = OutObj(Domain(Msg.Cmif(0, Bytes(0x11, 1)), creator));    // CreateLibraryApplet(SoftwareKeyboard, 后台模式)

                int Storage(byte[] data)
                {
                    int sto = OutObj(Domain(Msg.Cmif(10, Bytes((long)data.Length)), creator));   // CreateStorage
                    int acc = OutObj(Domain(Msg.Cmif(0), sto));                                   // IStorage.Open
                    data.CopyTo(new Span<byte>(buf, data.Length));
                    Msg w = Msg.Cmif(10, Bytes(0UL)); w.A.Add(((ulong)buf, (ulong)data.Length));   // IStorageAccessor.Write(offset 0)
                    Domain(w, acc);
                    return sto;
                }

                void Push(uint cmd, byte[] data) { Msg p = Msg.Cmif(cmd); p.InObjects.Add(Storage(data)); Domain(p, accessor); }

                // 游戏一次取空（PopInteractiveOutData 到 NotAvailable 为止），记「类型」，DecidedEnter / ChangedStringV2 带上文本
                string Drain()
                {
                    var got = new List<string>();
                    for (int i = 0; i < 16; i++)
                    {
                        Reply pop = Domain(Msg.Cmif(104), accessor);
                        if (pop.Result != 0) break;
                        int acc = OutObj(Domain(Msg.Cmif(0), OutObj(pop)));
                        long size = *(long*)Domain(Msg.Cmif(0), acc).Data;
                        new Span<byte>(buf, 0x2000).Clear();
                        Msg rd = Msg.Cmif(11, Bytes(0UL)); rd.B.Add(((ulong)buf, (ulong)Math.Min(size, 0x2000)));
                        Domain(rd, acc);
                        uint st = *(uint*)buf, type = *(uint*)(buf + 4);
                        string s = $"{type}@{st}";
                        if (type == 5)
                        {
                            int len = *(int*)(buf + 8 + 0x3EC);
                            s += "「" + new string((char*)(buf + 8), 0, Math.Clamp(len, 0, 0x3EC / 2)) + "」";
                        }
                        got.Add(s);
                    }
                    return got.Count == 0 ? "无" : string.Join(" ", got);
                }

                Push(100, Bytes(1u, 0x20u, 0u, 0u, 0UL, 0UL));                            // 公共参数
                Push(100, new byte[Marshal.SizeOf<SoftwareKeyboardInitialize>()]);       // 长度 = SoftwareKeyboardInitialize → 内嵌键盘
                Domain(Msg.Cmif(10), accessor);                                          // Start

                Push(103, Bytes(0xDu));                                                  // UseChangedStringV2
                var calc = new SoftwareKeyboardCalc
                {
                    Size = (ushort)Marshal.SizeOf<SoftwareKeyboardCalc>(),
                    Flags = KeyboardCalcFlags.Initialize | KeyboardCalcFlags.Appear | KeyboardCalcFlags.SetCursorPos,
                    Appear = new SoftwareKeyboardAppear { KeyboardMode = KeyboardMode.Alphabet, TextMinLength = 1, TextMaxLength = 4, OkText = "" },
                    InputText = "",
                };
                byte[] cb = new byte[Marshal.SizeOf<SoftwareKeyboardCalc>()];
                IntPtr p = Marshal.AllocHGlobal(cb.Length);
                try { Marshal.StructureToPtr(calc, p, false); Marshal.Copy(p, cb, 0, cb.Length); } finally { Marshal.FreeHGlobal(p); }
                Push(103, Bytes(0xAu, cb));                                              // Calc 0x15

                first = Drain();
                Thread.Sleep(1500);
                later = Drain();
                // 审计 P2-255：SetUserWordInfo 词数为负，照常回 ReleasedUserWordInfo（0xB = 11），不在回包前抛异常
                try { Push(103, Bytes(0x6u, -1)); negWords = Drain(); }
                catch (Exception e) { negWords = "抛异常：" + e.GetType().Name; }
            }
            catch (InvalidOperationException e)
            {
                first = later = $"应答解析失败：{e.Message}";
            }
            finally
            {
                FakeNative.Free(buf);
                FakeNative.HandleClose(oe);
                FakeNative.HandleClose(sm);
            }

            Check("内嵌键盘 Calc 0x15（Initialize | Appear）当批应答：不夹 DecidedEnter", "0@0 2@3 1@3", first);
            Check("内嵌键盘出现半秒后：先报改字 ChangedString（Shown），再 DecidedEnter → 回 Initialized；名字按最长 4 截断", "2@3 5@4「Play」 1@4 1@1", later);
            Check("词表词数为负（审计 P2-255）：照常回 ReleasedUserWordInfo", "True", (negWords.Contains("11@")).ToString() + (negWords.Contains("11@") ? "" : "（实得 " + negWords + "）"));
            Check("内嵌键盘交回文本补齐到最短：数字「0」最短 2 / 最长 3、「Player」最短 8", "00 / PlayerPlayer",
                $"{Ryujinx.HLE.HOS.Applets.SoftwareKeyboardApplet.FitLength("0", 2, 3)} / {Ryujinx.HLE.HOS.Applets.SoftwareKeyboardApplet.FitLength("Player", 8, 0)}");
        }
    }
}
