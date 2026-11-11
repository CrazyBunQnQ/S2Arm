// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import android.content.Context;
import android.os.Handler;
import java.util.ArrayList;
import java.util.List;

/** #48 P8：PowerStateSampler 的起停、只在变化时推、读不到推 -1（宿主替身，无设备）。 */
public final class PowerStateSamplerTest {
    private static int fails;
    private static void expect(String what, Object want, Object got) {
        System.out.println(what + " 期望 " + want + " → 真结果 " + got);
        if (!java.util.Objects.equals(want, got)) fails++;
    }

    public static void main(String[] args) {
        Context context = new Context();
        Handler handler = new Handler();
        List<String> reports = new ArrayList<>();
        PowerStateSampler sampler = new PowerStateSampler(context, handler,
            (thermal, save) -> reports.add(thermal + "/" + save));

        context.power.thermal = 3;   // SEVERE
        sampler.start();
        expect("start 排第一拍", true, handler.pending == sampler);
        handler.runPending();
        expect("第一拍推当前读数", "[3/false]", reports.toString());
        expect("下一拍 1 秒后", 1000L, handler.lastDelay);
        handler.runPending();
        expect("读数没变不再推", 1, reports.size());

        context.power.thermal = 4;   // CRITICAL
        handler.runPending();
        context.power.save = true;
        handler.runPending();
        expect("热状态变、省电变各推一次", "[3/false, 4/false, 4/true]", reports.toString());

        sampler.stop();
        expect("stop 撤掉待跑的拍", null, handler.pending);
        sampler.run();   // 撤销前已出队的一拍
        expect("停了之后的迟到拍不推", 3, reports.size());

        context.power.thermal = 2;   // 后台期间变了
        sampler.start();
        handler.runPending();
        expect("回前台第一拍推后台期间的变化", "2/true", reports.get(reports.size() - 1));
        handler.runPending();
        expect("回前台后没变不再推", 4, reports.size());

        context.power.throwThermal = true;
        context.power.throwSave = true;
        handler.runPending();
        expect("读数抛异常 → -1 / 省电关", "-1/false", reports.get(reports.size() - 1));
        sampler.stop();

        Context empty = new Context();
        empty.power = null;
        Handler h2 = new Handler();
        List<String> r2 = new ArrayList<>();
        PowerStateSampler none = new PowerStateSampler(empty, h2, (thermal, save) -> r2.add(thermal + "/" + save));
        none.start();
        h2.runPending();
        expect("没有 PowerManager → -1 / 省电关", "[-1/false]", r2.toString());

        System.out.println(fails == 0 ? "【绿】全部相符" : "【红】" + fails + " 项不符");
        if (fails != 0) System.exit(1);
    }
}
