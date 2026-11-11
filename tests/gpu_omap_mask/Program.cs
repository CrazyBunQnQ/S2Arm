// 颜色写掩码与片元着色器输出映射（OMAP）相与（compat/tree/src/Ryujinx.Graphics.Gpu/Engine/Threed/StateUpdater.cs S2AOmapColorMask，#468）。
// 期望值全部手算：输出映射每个渲染目标 4 位（RGBA），着色器没输出的分量写掩码清 0，其余保持游戏设的值。
using System;
using Ryujinx.Graphics.Gpu.Engine.Threed;

static class Program
{
    static int s_n, s_bad;

    static void Check(string name, uint expected, uint actual)
    {
        s_n++;
        if (expected == actual) return;
        s_bad++;
        Console.WriteLine($"✗ {name}：期望 0x{expected:X}，实得 0x{actual:X}");
    }

    static uint M(uint mask, uint omap, int rt) => StateUpdater.S2AOmapColorMask(mask, omap, rt);

    static int Main()
    {
        // ① 输出映射全 1（没有片元着色器 / 读不到，ShaderInfoBuilder 缺省 -1）：8 个目标都不动
        for (int rt = 0; rt < 8; rt++)
        {
            Check($"全 1 映射 RT{rt} 全掩码", 0xF, M(0xF, uint.MaxValue, rt));
            Check($"全 1 映射 RT{rt} 部分掩码 0x5", 0x5, M(0x5, uint.MaxValue, rt));
        }
        Check("-1 转 uint 等于全 1", 0xF, M(0xF, unchecked((uint)(-1)), 3));

        // ② 只有 RT1 有输出（映射 0x000000F0）：RT0 / RT2 整个目标不写，RT1 不动
        Check("只出 RT1：RT0 清 0", 0x0, M(0xF, 0x000000F0, 0));
        Check("只出 RT1：RT1 不动", 0xF, M(0xF, 0x000000F0, 1));
        Check("只出 RT1：RT2 清 0", 0x0, M(0xF, 0x000000F0, 2));

        // ③ 分量级：RT0 只输出 RGB（0x7）
        Check("RT0 只出 RGB：全掩码 → RGB", 0x7, M(0xF, 0x7, 0));
        Check("RT0 只出 RGB：R+A → 只剩 R", 0x1, M(0x9, 0x7, 0));
        Check("RT0 只出 RGB：只写 A → 0", 0x0, M(0x8, 0x7, 0));

        // ④ 游戏自己设 0 的保持 0
        Check("游戏掩码 0 不因映射变成非 0", 0x0, M(0x0, uint.MaxValue, 0));

        // ⑤ 最后一个目标 RT7：映射最高 4 位
        Check("RT7 映射 0xF0000000：RT7 不动", 0xF, M(0xF, 0xF0000000, 7));
        Check("RT7 映射 0xF0000000：RT6 清 0", 0x0, M(0xF, 0xF0000000, 6));
        Check("RT7 只出 G（0x20000000）", 0x2, M(0xF, 0x20000000, 7));

        Console.WriteLine($"{(s_bad == 0 ? "✓" : "✗")} gpu_omap_mask：{s_n} 项，没过 {s_bad}");
        return s_bad == 0 ? 0 : 1;
    }
}
