// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
package org.s2arm.android;

import android.content.Context;
import android.os.Handler;
import android.os.PowerManager;

/**
 * #48 P8：热状态与省电模式推给原生层（android/native/power_state.c → 共用接缝
 * dm_thermal_now / dm_low_power_now），主线程 1 秒一拍，只在游戏前台运行。
 * 共用层读的节奏也是每秒一次（runtime/src/fps.c 写入线程），所以用轮询，不注册监听：
 * 起停只跟 onResume / onPause，没有监听器的注销时序问题。
 * PowerManager 拿不到或读数抛异常时推 -1（没有读数）/ 省电关，同共用层弱缺省。
 */
final class PowerStateSampler implements Runnable {
    interface Sink { void report(int thermalStatus, boolean powerSave); }

    static final int UNAVAILABLE = -1;
    private static final long TICK_MS = 1000;
    private final PowerManager power;
    private final Handler handler;
    private final Sink sink;
    private boolean running;
    private int lastThermal = Integer.MIN_VALUE;   // 每个游戏局新建一个采样器：第一拍必推
    private int lastSave = -1;

    PowerStateSampler(Context context, Handler handler, Sink sink) {
        this.power = context.getSystemService(PowerManager.class);
        this.handler = handler;
        this.sink = sink;
    }

    void start() {
        if (running) return;
        running = true;
        handler.post(this);   // 原生层的值后台期间不变（只有这里写），回前台第一拍照常比对：变了才推
    }

    void stop() {
        if (!running) return;
        running = false;
        handler.removeCallbacks(this);
    }

    @Override public void run() {
        if (!running) return;
        int thermal = UNAVAILABLE;
        boolean save = false;
        if (power != null) {
            try {
                thermal = power.getCurrentThermalStatus();
            } catch (RuntimeException e) {
                thermal = UNAVAILABLE;
            }
            try {
                save = power.isPowerSaveMode();
            } catch (RuntimeException e) {
                save = false;
            }
        }
        int saveBit = save ? 1 : 0;
        if (thermal != lastThermal || saveBit != lastSave) {   // 只在变化时过 JNI
            lastThermal = thermal;
            lastSave = saveBit;
            sink.report(thermal, save);
        }
        handler.postDelayed(this, TICK_MS);
    }
}
