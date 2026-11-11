// GameSir G8+ MFi 有线版的实体马达走配件协议（#462，判定与组包见 ios/ea_rumble.h）。
//   EASession 的输入 / 输出流挂主线程 run loop，所有会话操作只在主线程；震动队列经 dispatch 到主线程提交，同一包不重发、只保留最新一包。
#import <ExternalAccessory/ExternalAccessory.h>
#import <UIKit/UIKit.h>
#import "ea_rumble.h"
#include "native.h"
#include <stdatomic.h>

static _Atomic int g_target;
static EASession *g_session;          // 以下只在主线程
static NSData *g_pending, *g_last;
static id<NSStreamDelegate> g_delegate;

static EAAccessory *find_accessory(void) {
    for (EAAccessory *a in EAAccessoryManager.sharedAccessoryManager.connectedAccessories)
        if (a.connected && s2a_gamesir_is_target(a.manufacturer.UTF8String, a.modelNumber.UTF8String, [a.protocolStrings containsObject:@S2A_GAMESIR_PROTOCOL])) return a;
    return nil;
}
static void close_session(void) {
    if (!g_session) return;
    for (NSStream *s in @[g_session.inputStream, g_session.outputStream]) { s.delegate = nil; [s close]; [s removeFromRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode]; }
    g_session = nil; g_pending = nil; g_last = nil;
}
static void flush(void) {
    NSOutputStream *out = g_session.outputStream;
    if (!g_pending || !out.hasSpaceAvailable) return;
    NSInteger n = [out write:g_pending.bytes maxLength:g_pending.length];
    if (n == (NSInteger)g_pending.length) { g_last = g_pending; g_pending = nil; return; }
    LOG("【红】手柄配件震动：写入 %ld / %lu 字节（%s），关会话，下次震动重开", (long)n, (unsigned long)g_pending.length, out.streamError.localizedDescription.UTF8String ?: "-");
    close_session();
}

@interface S2AEARumbleStream : NSObject <NSStreamDelegate> @end
@implementation S2AEARumbleStream
- (void)stream:(NSStream *)s handleEvent:(NSStreamEvent)e {
    if (e & NSStreamEventHasBytesAvailable) { uint8_t buf[64]; while ([(NSInputStream *)s hasBytesAvailable] && [(NSInputStream *)s read:buf maxLength:sizeof buf] > 0) {} }   // 手柄回的数据不用，读掉免得堵流
    if (e & NSStreamEventHasSpaceAvailable) flush();
    if (e & (NSStreamEventErrorOccurred | NSStreamEventEndEncountered)) { LOG("手柄配件震动：会话断开（%s）", s.streamError.localizedDescription.UTF8String ?: "流结束"); close_session(); }
}
@end

static BOOL open_session(void) {
    if (g_session) return YES;
    EAAccessory *a = find_accessory();
    if (!a) return NO;
    EASession *s = [[EASession alloc] initWithAccessory:a forProtocol:@S2A_GAMESIR_PROTOCOL];
    if (!s.inputStream || !s.outputStream) { LOG("【红】手柄配件震动：开不了 %s 会话", S2A_GAMESIR_PROTOCOL); return NO; }
    if (!g_delegate) g_delegate = [S2AEARumbleStream new];
    for (NSStream *st in @[s.inputStream, s.outputStream]) { st.delegate = g_delegate; [st scheduleInRunLoop:NSRunLoop.mainRunLoop forMode:NSDefaultRunLoopMode]; [st open]; }
    g_session = s;
    LOG("手柄配件震动：已开 %s 会话（%s %s，固件 %s）", S2A_GAMESIR_PROTOCOL, a.manufacturer.UTF8String ?: "?", a.modelNumber.UTF8String ?: "?", a.firmwareRevision.UTF8String ?: "?");
    return YES;
}

void s2a_ea_rumble_refresh(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        static BOOL registered;
        if (!registered) {
            registered = YES;
            [EAAccessoryManager.sharedAccessoryManager registerForLocalNotifications];
            for (NSNotificationName n in @[EAAccessoryDidConnectNotification, EAAccessoryDidDisconnectNotification])
                [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { (void)note; s2a_ea_rumble_refresh(); }];
            [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { (void)note; close_session(); }];
        }
        int was = atomic_load(&g_target), now = find_accessory() != nil;
        if (!now) close_session();
        atomic_store(&g_target, now);
        if (now != was) LOG("手柄配件震动：%s", now ? "接着 GameSir G8+ MFi（配件协议），游戏震动改发手柄马达" : "没有接着走配件协议的手柄，震动走系统手柄 / 手机马达");
    });
}
int s2a_ea_rumble_active(void) { return atomic_load(&g_target); }
void s2a_ea_rumble_set(float left, float right) {
    uint8_t p[9]; s2a_gamesir_packet(p, left, right);
    NSData *d = [NSData dataWithBytes:p length:sizeof p];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!open_session()) return;
        if (!g_pending && [d isEqualToData:g_last]) return;
        g_pending = d; flush();
    });
}
void s2a_ea_rumble_stop(void) { if (atomic_load(&g_target)) s2a_ea_rumble_set(0, 0); }
