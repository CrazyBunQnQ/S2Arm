// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// #48 P8：安卓的热状态 / 省电模式进共用接缝 dm_thermal_now / dm_low_power_now。
// 共用层（runtime/src/fps.c 的 60 帧降档、graphics/nvn/gfx/nvn/hle_nvn.c 的帧步门与帧日志）
// 只认这两个函数；iOS 强定义在 ios/app.m（NSProcessInfo），Mac 在 runtime/src/host_main.c，
// 安卓在 power_state.c。读数由 Java 侧 PowerStateSampler 每秒从 PowerManager 推下来。
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// 安卓 PowerManager.THERMAL_STATUS_*（API 29）→ 共用口径（iOS NSProcessInfoThermalState）：
//   0 正常 / 1 一般 / 2 严重 / 3 危急，-1 = 没有读数。
//   NONE 0 → 0；LIGHT 1、MODERATE 2 → 1（系统说体验不受大影响）；SEVERE 3 → 2（体验受明显影响）；
//   CRITICAL 4、EMERGENCY 5、SHUTDOWN 6 → 3（系统已尽力降功耗 / 要关部件）。其他值（含 -1）→ -1。
int s2a_android_thermal_level(int android_status);

// Java 推读数（任意线程）：android_status 原样传 PowerManager.getCurrentThermalStatus()，
// 读不到传 -1；power_save 非 0 = 省电模式开着。
void s2a_android_power_set(int android_status, int power_save);

int dm_thermal_now(void);     // 共用口径 0–3，起局前 / 没读数 = -1（同弱缺省）
int dm_low_power_now(void);   // 1 = 省电模式开着，没读数 = 0（同弱缺省）

#ifdef __cplusplus
}
#endif
