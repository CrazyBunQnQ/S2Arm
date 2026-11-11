// Joy-Con / Pro Controller 六轴：IOHID 直读（macOS GameController 对 Joy-Con 的 motion 恒为 nil）。
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#include "joycon_imu.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <mach/mach_time.h>

enum { JC_L = 0, JC_R = 1, JC_N = 2 };
enum { JC_PID_L = 0x2006, JC_PID_R = 0x2007, JC_PID_PRO = 0x2009 };

typedef struct {
    IOHIDDeviceRef device;
    uint8_t *report_buf;
    uint8_t pkt;
    int slot;                 // JC_L / JC_R
    int product_id;
    s2a_pad_motion sample;
    int known;
} jc_dev;

static IOHIDManagerRef g_mgr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static jc_dev g_dev[JC_N];
static _Atomic int g_want;
static int g_started;
static uint8_t g_pkt_global;

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static uint64_t now_us(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / (tb.denom * 1000ull);
}

static void send_sub(jc_dev *d, uint8_t cmd, uint8_t arg) {
    if (!d->device) return;
    uint8_t report[0x40];
    memset(report, 0, sizeof report);
    report[0] = 0x01;                         // OutputType.subcommand
    report[1] = (d->pkt++ & 0x0f);
    report[2] = 0x00; report[3] = 0x01; report[4] = 0x00; report[5] = 0x40;   // rumble neutral
    report[6] = 0x00; report[7] = 0x01; report[8] = 0x00; report[9] = 0x40;
    report[10] = cmd;
    report[11] = arg;
    IOHIDDeviceSetReport(d->device, kIOHIDReportTypeOutput, 0x01, report, sizeof report);
}

// Joy-Con 报告里的加速度 / 角速度 → 近似 NS2 原始坐标（g、度/秒）。
// 竖握：L/R 机身轴不同；Fit Boxing 横握挥拳时主要靠角速度幅值，先保证有读数。
static void decode_imu(int slot, int product_id, const uint8_t *s, s2a_pad_motion *out) {
    float ax = rd16(s + 0) * 0.000244f;
    float ay = rd16(s + 2) * 0.000244f;
    float az = rd16(s + 4) * 0.000244f;
    float gx = rd16(s + 6) * 0.06103f;
    float gy = rd16(s + 8) * 0.06103f;
    float gz = rd16(s + 10) * 0.06103f;
    // 映射到「正面法线 = z、上沿 = y、右 = x」的近似：与常见 SDL hidapi_nintendo 竖握一致
    if (product_id == JC_PID_L) {
        out->acceleration_x = ay; out->acceleration_y = -ax; out->acceleration_z = az;
        out->gyro_x = gy; out->gyro_y = -gx; out->gyro_z = gz;
    } else {
        // Joy-Con (R) / Pro
        out->acceleration_x = -ay; out->acceleration_y = ax; out->acceleration_z = az;
        out->gyro_x = -gy; out->gyro_y = gx; out->gyro_z = gz;
    }
    (void)slot;
    out->host_timestamp_us = now_us();
}

static void apply_report(jc_dev *d, uint32_t report_id, const uint8_t *report, CFIndex len) {
    if (report_id != 0x30 || !d) return;
    int off = (len > 0 && report[0] == 0x30) ? 1 : 0;
    if (len - off < 48) return;
    // 标准全报告带 3 组 IMU（5 ms 间隔）；取最后一组
    const uint8_t *s = report + off + 36;
    s2a_pad_motion m = {0};
    decode_imu(d->slot, d->product_id, s, &m);
    pthread_mutex_lock(&g_mu);
    d->sample = m;
    d->known = 1;
    pthread_mutex_unlock(&g_mu);
}

static void report_cb(void *context, IOReturn result, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex reportLength) {
    (void)result; (void)sender; (void)type;
    apply_report((jc_dev *)context, reportID, report, reportLength);
}

static int slot_for_pid(int pid) {
    if (pid == JC_PID_L) return JC_L;
    if (pid == JC_PID_R || pid == JC_PID_PRO) return JC_R;
    return -1;
}

static void attach_device(IOHIDDeviceRef device) {
    CFNumberRef pid_cf = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    int pid = 0;
    if (pid_cf) CFNumberGetValue(pid_cf, kCFNumberIntType, &pid);
    int slot = slot_for_pid(pid);
    if (slot < 0) return;

    pthread_mutex_lock(&g_mu);
    jc_dev *d = &g_dev[slot];
    if (d->device == device) { pthread_mutex_unlock(&g_mu); return; }
    if (d->device) {
        IOHIDDeviceRegisterInputReportCallback(d->device, d->report_buf, 0x200, NULL, NULL);
        IOHIDDeviceClose(d->device, kIOHIDOptionsTypeNone);
        free(d->report_buf);
        memset(d, 0, sizeof *d);
    }
    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    d->device = device;
    d->product_id = pid;
    d->slot = slot;
    d->pkt = g_pkt_global++;
    d->report_buf = calloc(1, 0x200);
    if (!d->report_buf) {   // 审计 P2-271：分配失败不能按 0x200 字节登记回调（报告来了写进空指针）；这只手柄本次不接，关掉设备、槽位清空
        IOHIDDeviceClose(device, kIOHIDOptionsTypeNone); memset(d, 0, sizeof *d);
        pthread_mutex_unlock(&g_mu);
        fprintf(stderr, "【红】Joy-Con 体感：读报告缓冲分配失败，这只手柄本次不接体感\n");
        return;
    }
    IOHIDDeviceRegisterInputReportCallback(device, d->report_buf, 0x200, report_cb, d);
    int want = atomic_load(&g_want);
    pthread_mutex_unlock(&g_mu);

    if (want) {
        send_sub(d, 0x40, 1);     // enable IMU
        send_sub(d, 0x03, 0x30);  // standard full
        fprintf(stderr, "[输入] Joy-Con HID 六轴：已接通 %s（shared）\n",
                pid == JC_PID_L ? "L" : (pid == JC_PID_PRO ? "Pro" : "R"));
    }
}

static void detach_device(IOHIDDeviceRef device) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < JC_N; i++) {
        jc_dev *d = &g_dev[i];
        if (d->device != device) continue;
        IOHIDDeviceRegisterInputReportCallback(d->device, d->report_buf, 0x200, NULL, NULL);
        IOHIDDeviceClose(d->device, kIOHIDOptionsTypeNone);
        free(d->report_buf);
        memset(d, 0, sizeof *d);
    }
    pthread_mutex_unlock(&g_mu);
}

static void match_cb(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    attach_device(device);
}

static void remove_cb(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    detach_device(device);
}

static void ensure_manager(void) {
    if (g_started) return;
    g_started = 1;
    g_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    NSArray *match = @[
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @(JC_PID_L)},
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @(JC_PID_R)},
        @{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @(JC_PID_PRO)},
    ];
    IOHIDManagerSetDeviceMatchingMultiple(g_mgr, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_mgr, match_cb, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_mgr, remove_cb, NULL);
    IOHIDManagerScheduleWithRunLoop(g_mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(g_mgr, kIOHIDOptionsTypeNone);
    NSSet *devs = CFBridgingRelease(IOHIDManagerCopyDevices(g_mgr));
    for (id obj in devs) attach_device((__bridge IOHIDDeviceRef)obj);
}

static void apply_enabled_locked(int enabled) {
    for (int i = 0; i < JC_N; i++) {
        jc_dev *d = &g_dev[i];
        if (!d->device) continue;
        if (enabled) {
            send_sub(d, 0x40, 1);
            send_sub(d, 0x03, 0x30);
        } else {
            send_sub(d, 0x40, 0);
            send_sub(d, 0x03, 0x3f);
            d->known = 0;
            memset(&d->sample, 0, sizeof d->sample);
        }
    }
}

void s2a_joycon_imu_set_enabled(int enabled) {
    ensure_manager();
    int on = enabled ? 1 : 0;
    int prev = atomic_exchange(&g_want, on);
    if (prev == on) return;
    pthread_mutex_lock(&g_mu);
    apply_enabled_locked(on);
    pthread_mutex_unlock(&g_mu);
    if (on) fprintf(stderr, "[输入] Joy-Con HID 六轴：开启（GameController 无 motion 时的回退）\n");
    else fprintf(stderr, "[输入] Joy-Con HID 六轴：关闭\n");
}

int s2a_joycon_imu_read(s2a_pad_motion *out_l, s2a_pad_motion *out_r) {
    int mask = 0;
    pthread_mutex_lock(&g_mu);
    if (g_dev[JC_L].known) {
        if (out_l) *out_l = g_dev[JC_L].sample;
        mask |= 1;
    }
    if (g_dev[JC_R].known) {
        if (out_r) *out_r = g_dev[JC_R].sample;
        mask |= 2;
    }
    pthread_mutex_unlock(&g_mu);
    return mask;
}
