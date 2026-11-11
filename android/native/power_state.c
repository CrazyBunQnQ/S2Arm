// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
//
// #48 P8：dm_thermal_now / dm_low_power_now 的安卓强定义（盖过 fps.c / hle_nvn.c 的弱缺省 -1 / 0）。
// 只存两个原子量：Java 写、共用层读（fps.c 写入线程每秒一次，hle_nvn.c 帧步与帧日志）。
#include "power_state.h"
#include <stdatomic.h>

static _Atomic int g_thermal = -1;
static _Atomic int g_power_save = 0;

int s2a_android_thermal_level(int android_status) {
    switch (android_status) {
    case 0: return 0;
    case 1: case 2: return 1;
    case 3: return 2;
    case 4: case 5: case 6: return 3;
    default: return -1;
    }
}

void s2a_android_power_set(int android_status, int power_save) {
    atomic_store_explicit(&g_thermal, s2a_android_thermal_level(android_status), memory_order_relaxed);
    atomic_store_explicit(&g_power_save, power_save != 0, memory_order_relaxed);
}

int dm_thermal_now(void) { return atomic_load_explicit(&g_thermal, memory_order_relaxed); }
int dm_low_power_now(void) { return atomic_load_explicit(&g_power_save, memory_order_relaxed); }
