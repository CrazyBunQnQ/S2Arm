// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// 外接显示器（转接线 / AirPlay）原生全屏，iOS 27 起。系统缺省把手机屏镜像到外接屏；外接屏场景里有了可见窗口，这块屏只显示这个窗口。
//   用 iOS 27 SDK 链接的 App，系统不再自动给外接屏场景（Info.plist 里声明场景角色也不给，11-10 iPhone 18 Pro 实测）：要在主界面的视图控制器上
//   登记场景配件（UISceneAccessory），控制器在屏上、登记启用、显示器接着时系统才连场景。s2a_ios_ext_register 由游戏界面控制器 S2AGameVC
//   建好时调（ios/app.m）；游戏库界面不登记 = 照旧镜像。iOS 18–26 没有这套接口，照旧镜像。
//   窗口到直连出图层要挂时才建（s2a_ios_present_view，graphics/nvn/host_adapt.m 调）：加载页期间没有出图层，外接屏照旧镜像手机屏。
//   出图层在外接屏上时，手机屏留触屏手柄 / 提示页 / 开发浮层；屏幕比例改按外接屏算（ios/app.m screen_ratio），拔掉后出图层回手机屏。
//   开关：S2A_EXT_DISPLAY=0 = 不登记、不接管外接屏（照旧镜像）。
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include "native.h"
UIView *s2a_ios_root_view(void);                               // app.m
void s2a_direct_relayout(void) __attribute__((weak_import));   // graphics/nvn/host_adapt.m：出图层按当前出图视图重挂 / 重摆（没链直连层的壳没有这个符号）

static UIWindowScene *g_scene;   // 外接屏场景（主线程）；nil = 没接
static UIWindow *g_window;       // 外接屏窗口（主线程）；出图层第一次要挂时才建
static _Atomic double g_ratio;   // 外接屏 宽 ÷ 高；0 = 没接。外接屏不随手机转向，不取长边 ÷ 短边（竖放的显示器按窄屏加高）
double s2a_ios_ext_ratio(void) { return g_ratio; }
static void note_ratio(CGSize z) { if (z.width > 0 && z.height > 0) g_ratio = z.width / z.height; }
static int ext_on(void) { const char *e = getenv("S2A_EXT_DISPLAY"); return !(e && !strcmp(e, "0")); }

@interface S2AExternalVC : UIViewController
@end
@implementation S2AExternalVC
- (void)viewDidLayoutSubviews {   // 外接屏换了显示模式：比例跟着换，出图层重摆
    [super viewDidLayoutSubviews];
    if (!g_window || self != g_window.rootViewController) return;   // 断开后迟到的布局回调不许把比例写回去
    note_ratio(self.view.bounds.size);
    if (s2a_direct_relayout) s2a_direct_relayout();
}
@end

// 出图层该挂的视图（主线程）：外接屏接着 = 外接屏窗口的视图（这时才建窗口，系统停止镜像），否则 App 根视图
UIView *s2a_ios_present_view(void) {
    if (!g_scene) return s2a_ios_root_view();
    if (!g_window) {
        UIWindow *w = [[UIWindow alloc] initWithWindowScene:g_scene];
        UIViewController *vc = [S2AExternalVC new]; vc.view.backgroundColor = UIColor.blackColor;
        w.rootViewController = vc; g_window = w;   // 先记下再显示：显示时的布局回调会回到 s2a_direct_relayout → 本函数
        w.hidden = NO; vc.view.frame = w.bounds;
        note_ratio(w.bounds.size);
        LOG("外接屏：窗口已建 %.0fx%.0f 点，接管这块屏（不再镜像手机屏）", w.bounds.size.width, w.bounds.size.height);
    }
    return g_window.rootViewController.view;
}

@interface S2AExternalSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end
@implementation S2AExternalSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    UIWindowScene *ws = (UIWindowScene *)scene; UIScreen *sc = ws.screen;
    // 过扫描补偿不是「不补偿」时（UIScreen.h 写的缺省是缩放：整幅画面缩小一圈、四周留黑）改成不补偿，画面 1:1 铺满显示器。
    //   11-10 iPhone 18 Pro / iOS 27 接 2560x1440 显示器，连上时读到的已是 2（不补偿），这一行没有动作
    if (sc.overscanCompensation != UIScreenOverscanCompensationNone) {
        LOG("外接屏：过扫描补偿 %ld 改成不补偿", (long)sc.overscanCompensation); sc.overscanCompensation = UIScreenOverscanCompensationNone; }
    CGSize z = sc.bounds.size, n = sc.nativeBounds.size, m = sc.currentMode.size;
    g_scene = ws; note_ratio(z);
    LOG("外接屏：已连上 %.0fx%.0f 点、%.0fx%.0f 像素（倍数 %.2f，显示模式 %.0fx%.0f，最高 %ld Hz，比例 %.3f）；出图层挂上后接管，之前照旧镜像",
        z.width, z.height, n.width, n.height, sc.nativeScale, m.width, m.height, (long)sc.maximumFramesPerSecond, (double)g_ratio);
    if (s2a_direct_relayout) s2a_direct_relayout();   // 局中接上：出图层挪过来
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    if (scene != g_scene) return;
    UIWindow *w = g_window; g_scene = nil; g_window = nil; g_ratio = 0;
    LOG("外接屏：已断开");
    if (s2a_direct_relayout) s2a_direct_relayout();   // 先把出图层挪回手机屏，再放掉窗口
    w.hidden = YES;
}
@end

// 游戏界面控制器建好时登记外接屏场景配件（主线程）。返回登记对象，由控制器持有（控制器放掉 = 登记跟着没）；iOS 27 以下 / 不接管返回 nil
id s2a_ios_ext_register(UIViewController *vc) {
    if (!vc || !ext_on()) return nil;
    if (@available(iOS 27.0, *)) {
        // 委托类写在代码里、不取配置名：Info.plist 里不用声明外接屏场景
        UISceneConfiguration *cfg = [[UISceneConfiguration alloc] initWithName:nil sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
        cfg.delegateClass = S2AExternalSceneDelegate.class;
        UISceneAccessoryRegistration *reg = [vc registerSceneAccessory:[UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:cfg]];
        LOG("外接屏：场景配件已登记（启用 %d，显示器可用 %d）", (int)reg.enabled, (int)reg.available);
        return reg;
    }
    return nil;
}
