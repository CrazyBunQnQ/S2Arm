// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// #48 P8：安卓 dm_thermal_now / dm_low_power_now 强定义（android/native/power_state.c）。
// 这里照抄共用层的弱缺省（runtime/src/fps.c:68-69、graphics/nvn/gfx/nvn/hle_nvn.c:3042-3043），
// 和生产链接同一个局面：链了 power_state.c 就是它的强定义，没链就落到弱缺省 -1 / 0。
// 不链 power_state.c（main 上、或负对照）时，推了「危急 + 省电」读到的仍是 -1 / 0 → 红。
// 期望值按 power_state.h 的映射表手写，不调被测实现算。
#include <stdio.h>

__attribute__((weak)) int dm_thermal_now(void) { return -1; }
__attribute__((weak)) int dm_low_power_now(void) { return 0; }
__attribute__((weak)) int s2a_android_thermal_level(int android_status);
__attribute__((weak)) void s2a_android_power_set(int android_status, int power_save);

static int fails;
static void expect(const char *what, int want, int got) {
    printf("%s 期望 %d → 真结果 %d\n", what, want, got);
    if (want != got) fails++;
}

int main(void) {
    expect("起局前热状态（没读数）", -1, dm_thermal_now());
    expect("起局前省电（没读数）", 0, dm_low_power_now());
    if (!s2a_android_power_set || !s2a_android_thermal_level) {
        printf("【红】没链到安卓的读数入口 s2a_android_power_set：共用层只能读到弱缺省\n");
        fails++;
    } else {
        // 映射表（PowerManager.THERMAL_STATUS_* → 共用 0–3）：
        static const struct { int status, level; const char *name; } map[] = {
            {0, 0, "NONE"}, {1, 1, "LIGHT"}, {2, 1, "MODERATE"}, {3, 2, "SEVERE"},
            {4, 3, "CRITICAL"}, {5, 3, "EMERGENCY"}, {6, 3, "SHUTDOWN"},
            {-1, -1, "读不到"}, {7, -1, "未知 7"}, {-2, -1, "未知 -2"},
        };
        char what[64];
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
            snprintf(what, sizeof what, "映射 %s(%d)", map[i].name, map[i].status);
            expect(what, map[i].level, s2a_android_thermal_level(map[i].status));
        }
    }
    // 经接缝读回：共用层（fps.c 降档、hle_nvn.c 帧步）看到的就是这两个值
    if (s2a_android_power_set) s2a_android_power_set(4, 1);
    expect("推 CRITICAL → 共用层读到危急", 3, dm_thermal_now());
    expect("推省电开 → 共用层读到低电量", 1, dm_low_power_now());
    if (s2a_android_power_set) s2a_android_power_set(3, 0);
    expect("推 SEVERE → 共用层读到严重（60 帧不降，11-06 口径）", 2, dm_thermal_now());
    expect("推省电关 → 立即读回 0", 0, dm_low_power_now());
    if (s2a_android_power_set) s2a_android_power_set(-1, 7);
    expect("读不到热状态 → 回 -1", -1, dm_thermal_now());
    expect("省电非 0 一律当开", 1, dm_low_power_now());
    printf(fails ? "【红】%d 项不符\n" : "【绿】全部相符\n", fails);
    return fails ? 1 : 0;
}
