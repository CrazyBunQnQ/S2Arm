// Vulkan 后端（nvn_gpu_*）D8 第一段验收：直接调 D0 原语，不经过任何 OP 解码器。
// 覆盖设计文档 §8：C01（32 位填充）、C02/C03（失败终止、失败段不回写）、C04（发布顺序）、
// C05/C06（录制未提交保活）、C08（cmd_abort）、C23 的粘滞错误部分，以及 close 义务。
// 期望值全部是 CPU 手算 / 契约条文，不取自被测实现。
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvn_backend.h"
#include "nvn_vk_format.h"
#include "nvn_fmt_info.h"
#include "vk_device.h"

// C66 only: model a strict driver enforcing maxSets=64; all other calls
// reach the real lavapipe entry. This is injected behaviour, not a claim about llvmpipe.
static int g_c66_inject_pool_limit;
static unsigned g_c66_sets_requested;
static VkDescriptorPool g_c66_pool;
VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets) {
    if (g_c66_inject_pool_limit) {
        if (g_c66_pool != info->descriptorPool) { g_c66_pool = info->descriptorPool; g_c66_sets_requested = 0; }
        g_c66_sets_requested += info->descriptorSetCount;
        if (g_c66_sets_requested > 64) return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    return __real_vkAllocateDescriptorSets(device, info, sets);
}

// 限定注入：真实 R32_UINT 支持不变，仅模拟驱动只报告单一 texel 能力。
static VkFormatFeatureFlags g_texel_feature_mask;
VKAPI_ATTR void VKAPI_CALL __real_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice device, VkFormat format, VkFormatProperties *props) {
    __real_vkGetPhysicalDeviceFormatProperties(device, format, props);
    if (g_texel_feature_mask && format == VK_FORMAT_R32_UINT)
        props->bufferFeatures &= ~g_texel_feature_mask;
}

extern uint32_t nvn_gpu_vk_test_push_capacity(nvn_gpu_cmd c, nvn_gpu_stage st);
extern void nvn_gpu_vk_test_set_fault(int mode);
extern void nvn_gpu_vk_test_fail_fence(uint64_t signal);
extern void nvn_gpu_vk_test_delay_segment(uint64_t signal, uint32_t ms);

static int g_fail;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            g_fail++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)
#define OK(r) ((r).code == NVN_GPU_OK)

struct done_rec { uint64_t signal; uint32_t status; int code; };
static struct done_rec g_done[64];
static atomic_int g_ndone;
static atomic_int g_order_bad;
static volatile int g_event;   // 普通变量：C04 靠 fence_wait 的同步边，TSan 才能真的检查
static atomic_int g_retired;
static atomic_int g_ndone_at_retire;
static atomic_int g_probe_order, g_order_code;

static void on_done(void *u, const nvn_gpu_done *d) {
    (void)u;
    int i = atomic_load_explicit(&g_ndone, memory_order_relaxed);   // done 回调由 worker 串行调用
    if (i < 64) { g_done[i].signal = d->signal; g_done[i].status = d->status; g_done[i].code = d->rc.code; }
    atomic_store_explicit(&g_ndone, i + 1, memory_order_release);
    // 发布顺序：成功段的 done 里，已完成值必须还没推进到本段 signal。
    if (d->status == NVN_GPU_DONE_OK && nvn_gpu_fence_done() >= d->signal) atomic_fetch_add(&g_order_bad, 1);
    if (d->status == NVN_GPU_DONE_OK) g_event = 9;
    if (atomic_load(&g_probe_order)) {
        nvn_gpu_err e;
        atomic_store(&g_order_code, nvn_gpu_fence_wait(d->signal, 0, &e).code);
    }
}

static void on_retire(void *u) {
    (void)u;
    atomic_store(&g_ndone_at_retire, atomic_load(&g_ndone));
    atomic_fetch_add(&g_retired, 1);
}

static void sleep_ms(int ms) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&ts, NULL); }

static int wait_until(atomic_int *v, int target, int ms) {
    for (int i = 0; i < ms; i++) {
        if (atomic_load(v) >= target) return 1;
        sleep_ms(1);
    }
    return atomic_load(v) >= target;
}

static void wait_done(int n) { CHECK(wait_until(&g_ndone, n, 5000), "等 done 回调超时：期望 %d，实到 %d", n, atomic_load(&g_ndone)); }

// 只认 base 之后的新记录：重开会话后 signal 会从头开始，旧记录不能冒充。
static const struct done_rec *wait_signal(uint64_t signal, int base, int ms) {
    for (int t = 0; t < ms; t++) {
        int n = atomic_load_explicit(&g_ndone, memory_order_acquire);
        if (n > 64) n = 64;
        for (int i = n - 1; i >= base; i--)
            if (g_done[i].signal == signal) return &g_done[i];
        sleep_ms(1);
    }
    return NULL;
}

static uint8_t *buf_cpu(nvn_gpu_buf b) { return nvn_gpu_buf_cpu(b); }

static struct s2a_vk_device g_dev;
static nvn_gpu_open_desc g_desc;

static int open_backend(void) {
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    nvn_gpu_rc r = nvn_gpu_open(&g_desc, &e);
    if (!OK(r)) printf("open 失败：%d %s\n", r.code, e.why);
    return OK(r);
}

static nvn_gpu_buf make_buf(uint64_t n, int mem, const void *init) {
    nvn_gpu_buf b = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_buf_create(n, mem, init, &b, &e);
    CHECK(OK(r), "buf_create(%llu) 失败：%d %s", (unsigned long long)n, r.code, e.why);
    return b;
}

static nvn_gpu_tex make_tex(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = NVN_TEXTURE_TARGET_2D;
    td.d.format = (int)fmt;
    td.d.width = w;
    td.d.height = h;
    td.d.depth = 1;
    td.d.levels = 1;
    td.d.samples = 0;
    td.usage = usage;
    nvn_gpu_tex t = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create 失败：%d %s", r.code, e.why);
    return t;
}

static nvn_gpu_tex make_tex_target(uint32_t w, uint32_t h, uint32_t depth, uint32_t target, uint32_t fmt, uint32_t usage) {
    nvn_gpu_tex_desc td;
    memset(&td, 0, sizeof td);
    td.d.target = (int)target; td.d.format = (int)fmt; td.d.width = w; td.d.height = h;
    td.d.depth = (int)depth; td.d.levels = 1; td.d.samples = 0; td.usage = usage;
    nvn_gpu_tex t = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_tex_create(&td, &t, &e);
    CHECK(OK(r), "tex_create target %u 失败：%d %s", target, r.code, e.why);
    return t;
}

static nvn_gpu_view make_view(nvn_gpu_tex t) {
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, NULL, &v, &e);
    CHECK(OK(r), "view_create 失败：%d %s", r.code, e.why);
    return v;
}

static nvn_gpu_cmd begin_cmd(void) {
    nvn_gpu_cmd c = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_begin(NULL, &c, &e);
    CHECK(OK(r), "cmd_begin 失败：%d %s", r.code, e.why);
    return c;
}

// C01：fill 是 32 位样式，不是逐字节 memset；范围外不动。
static void t01_fill32(void) {
    uint8_t zeros[64];
    memset(zeros, 0, sizeof zeros);
    nvn_gpu_buf b = make_buf(sizeof zeros, NVN_GPU_MEM_SHARED, zeros);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 16, 16, 0x11223344u);
    nvn_gpu_err e;
    atomic_store(&g_probe_order, 1);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 2000000000ull, &e);
    CHECK(OK(r), "wait 1：%d %s", r.code, e.why);
    uint8_t *p = buf_cpu(b);
    CHECK(p != NULL, "SHARED 缓冲没有 CPU 地址");
    static const uint8_t want[4] = {0x44, 0x33, 0x22, 0x11};
    for (int i = 0; i < 16; i++) CHECK(p[16 + i] == want[i % 4], "fill 字节 %d = %02x，期望 %02x", 16 + i, p[16 + i], want[i % 4]);
    for (int i = 0; i < 16; i++) CHECK(p[i] == 0, "fill 前越界改了字节 %d", i);
    for (int i = 32; i < 64; i++) CHECK(p[i] == 0, "fill 后越界改了字节 %d", i);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(1);
}

// 写入 + 拷贝：dst 与 src 全等；CPU 真值不来自 GPU。
static void t02_write_copy(void) {
    uint8_t init[256], zero[256];
    for (int i = 0; i < 256; i++) init[i] = (uint8_t)i;
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(256, NVN_GPU_MEM_SHARED, init);
    nvn_gpu_buf b = make_buf(256, NVN_GPU_MEM_SHARED, zero);
    uint8_t w[16];
    for (int i = 0; i < 16; i++) w[i] = (uint8_t)(0x80 + i);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_write(c, a, 32, w, sizeof w);
    nvn_gpu_copy_buf(c, a, 0, b, 0, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(a), buf_cpu(b), 256) == 0, "copy 后 dst 与 src 不同");
    CHECK(memcmp(buf_cpu(a) + 32, w, sizeof w) == 0, "buf_write 内容不对");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(2);
}

// C05：录制进未提交 cmd 后立刻 release，真正销毁必须等提交段 done 之后。
static void t03_keepalive_unsubmitted(void) {
    uint8_t pat[64], zero[64];
    memset(pat, 0xAB, sizeof pat);
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf src = make_buf(64, NVN_GPU_MEM_SHARED, pat);
    nvn_gpu_buf dst = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_copy_buf(c, src, 0, dst, 0, 64);
    nvn_gpu_buf_release(src, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "录制未提交的 src 被提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(dst)[i] == 0xAB, "保活期间内容被改，字节 %d", i);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "commit 完成后 src 没有退休");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_base + 1, "retired 在本段 done 之前就调了");
    nvn_gpu_buf_release(dst, NULL, NULL);
    wait_done(3);
}

// C06：经 GPU 地址间接引用（buf_use）也保活；retired 在该段 done 之后才调。
static void t03b_buf_use(void) {
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, zero);
    int retired_before = atomic_load(&g_retired), done_before = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_buf_use(c, b);
    nvn_gpu_buf_release(b, on_retire, NULL);
    sleep_ms(20);
    CHECK(atomic_load(&g_retired) == retired_before, "buf_use 录制后仍提前销毁");
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "buf_use 段完成后没有退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "retired 调了不止一次");
    CHECK(atomic_load(&g_ndone_at_retire) >= done_before + 1, "retired 在本段 done 之前就调了");
    wait_done(4);
}

// C08：abort 不执行、不回调、不推进时间线，引用随即解除。
static void t04_abort(void) {
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf keep = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf rel = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    int done_before = atomic_load(&g_ndone), retired_before = atomic_load(&g_retired);
    uint64_t fence_before = nvn_gpu_fence_done();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, keep, 0, 64, 0xFFFFFFFFu);
    nvn_gpu_copy_buf(c, keep, 0, rel, 0, 64);
    nvn_gpu_buf_release(rel, on_retire, NULL);
    nvn_gpu_cmd_abort(c);
    sleep_ms(50);
    for (int i = 0; i < 64; i++) CHECK(buf_cpu(keep)[i] == 0, "abort 后仍执行了 fill（字节 %d）", i);
    CHECK(atomic_load(&g_ndone) == done_before, "abort 后仍调了 done");
    CHECK(nvn_gpu_fence_done() == fence_before, "abort 推进了时间线");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "abort 后引用没有解除 / 没有退休");
    nvn_gpu_buf_release(keep, NULL, NULL);
}

// C02/C03：失败段终止后续；失败段与后续段都不执行、各回调一次 FAILED。
static void t05_failure_terminal(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;

    // 三段都先录好再依次提交：失败终止后 begin 会返回 E_FAILED，C02 的「段 3」属于已录制的段。
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);

    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(OK(r), "段 1 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(5, 2000000000ull, &e);
    CHECK(OK(r), "wait(5)：%d %s", r.code, e.why);

    nvn_gpu_vk_test_set_fault(1);
    r = nvn_gpu_cmd_commit(c2, 6, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_DEVICE, "段 2 注入失败应返回 E_DEVICE，实为 %d", r.code);

    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "段 3 应返回 E_FAILED，实为 %d", r.code);

    CHECK(nvn_gpu_fence_done() == 5, "fence_done 应为 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_failed() == 6, "fence_failed 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    r = nvn_gpu_fence_wait(6, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED，实为 %d", r.code);
    r = nvn_gpu_fence_wait(7, 2000000000ull, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "wait(7) 应 FAILED，实为 %d", r.code);

    const struct done_rec *d2 = wait_signal(6, 0, 5000), *d3 = wait_signal(7, 0, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    for (int i = 0; i < 16; i++) {
        CHECK(((uint32_t *)buf_cpu(b))[i / 4] == 0, "失败段写了 B（偏移 %d）", i);
        CHECK(((uint32_t *)buf_cpu(c))[i / 4] == 0, "失败段之后仍写了 C（偏移 %d）", i);
    }
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_FAILED, "失败终态后 cmd_begin 应 E_FAILED");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C04：done 里写的值，等 fence 的线程返回后一定读得到。
struct waiter_arg { uint64_t v; int ok; };
static void *waiter_main(void *p) {
    struct waiter_arg *a = p;
    nvn_gpu_err e;
    a->ok = OK(nvn_gpu_fence_wait(a->v, 5000000000ull, &e));
    return NULL;
}

static void t06_publish_order(void) {
    g_event = 0;
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x00000009u);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    struct waiter_arg a = {1, 0};
    pthread_t t;
    pthread_create(&t, NULL, waiter_main, &a);
    pthread_join(t, NULL);
    CHECK(a.ok, "wait(1) 失败");
    CHECK(atomic_load(&g_order_code) == NVN_GPU_E_ORDER, "done 里 fence_wait 应 E_ORDER，实为 %d", atomic_load(&g_order_code));
    atomic_store(&g_probe_order, 0);
    CHECK(g_event == 9, "done 的写没有在 fence 返回前可见（读到 %d）", g_event);
    CHECK(atomic_load(&g_order_bad) == 0, "done 被调时已完成值已推进（发布顺序错）");
    nvn_gpu_buf_release(b, NULL, NULL);
    wait_done(4);
}

// 等待语义：从没提交过的值 E_ARG；纯查询不产生副作用。
static void t07_wait_args(void) {
    nvn_gpu_err e;
    CHECK(nvn_gpu_fence_wait(1, 0, &e).code == NVN_GPU_OK, "wait(1, 0) 应 OK");
    CHECK(nvn_gpu_fence_wait(999, 0, &e).code == NVN_GPU_E_ARG, "wait(999) 应 E_ARG");
    CHECK(nvn_gpu_fence_wait(0, 0, &e).code == NVN_GPU_E_ARG, "wait(0) 应 E_ARG");
}

// 粘滞错误：整段不提交，按失败段处理（F = 该段 signal）。
static void t08_sticky(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 6, 0xDEADBEEFu);   // 非 4 字节长度
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "粘滞段 commit 应返回 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    const struct done_rec *d5 = wait_signal(2, base, 5000);
    CHECK(d5 && d5->status == NVN_GPU_DONE_FAILED, "粘滞段回调不是 FAILED");
    for (int i = 0; i < 16; i++) CHECK(buf_cpu(b)[i] == 0, "粘滞段仍执行了");
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 没有引用时 release 立即退休。
static void t09_release_only(void) {
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf b = make_buf(32, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf_release(b, on_retire, NULL);
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "无引用 release 没有退休");
}

// 清屏：CPU 手算像素；先清屏再读回，布局屏障也一起走通。
static void t11_clear_color(void) {
    nvn_gpu_tex t = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_RENDER | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    CHECK(nvn_gpu_buf_create(0, NVN_GPU_MEM_SHARED, NULL, &(nvn_gpu_buf){0}, NULL).code != 0, "空缓冲应被拒");
    nvn_gpu_cmd c = begin_cmd();
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_clear_view(c, v, red, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 32, 256);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(2, 2000000000ull, &e);
    CHECK(OK(r), "wait 2：%d %s", r.code, e.why);
    for (int i = 0; i < 64; i++) {
        const uint8_t want[4] = {0xFF, 0x00, 0x00, 0xFF};
        CHECK(buf_cpu(b)[i * 4 + 0] == want[0] && buf_cpu(b)[i * 4 + 1] == want[1] &&
              buf_cpu(b)[i * 4 + 2] == want[2] && buf_cpu(b)[i * 4 + 3] == want[3],
              "清屏像素 %d = %02x%02x%02x%02x", i, buf_cpu(b)[i * 4], buf_cpu(b)[i * 4 + 1], buf_cpu(b)[i * 4 + 2], buf_cpu(b)[i * 4 + 3]);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 上传 + 贴图到贴图复制：读回字节与 CPU 真值逐字节相同。
static void t12_upload_copy_tex(void) {
    uint8_t pat[4 * 4 * 4];
    for (int i = 0; i < (int)sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 3);
    nvn_gpu_tex src = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex dst = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, src, 0, 0, NULL, pat, 16, 64);
    nvn_gpu_copy_tex(c, src, 0, 0, NULL, dst, 0, 0, NULL);
    nvn_gpu_copy_tex_buf(c, dst, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(3, 2000000000ull, &e);
    CHECK(OK(r), "wait 3：%d %s", r.code, e.why);
    CHECK(memcmp(buf_cpu(b), pat, sizeof pat) == 0, "上传 + 复制后的像素与 CPU 真值不同");
    nvn_gpu_tex_release(src);
    nvn_gpu_tex_release(dst);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// 深度清屏：0.5 的 D32_SFLOAT 是 0x3F000000，直接对字节。
static void t13_clear_depth(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const float half[4] = {0.5f, 0.0f, 0.0f, 0.0f};
    nvn_gpu_clear_view(c, v, half, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 4, on_done, NULL, &e);
    CHECK(OK(r), "commit 4：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(4, 2000000000ull, &e);
    CHECK(OK(r), "wait 4：%d %s", r.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x3F000000u, "深度像素 %d = 0x%08x，期望 0x3f000000", i, word);
    }
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    void *p = malloc((size_t)sz);
    if (p && fread(p, 1, (size_t)sz, f) != (size_t)sz) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = (size_t)sz;
    return p;
}

// 原创纹理三角形：全屏三角形 + 2x2 纹理 + nearest 采样；逐像素用 CPU 手算真值。
static void tri_impl(const char *shader_dir, uint32_t tex_fmt, const uint8_t *texels, int tbpp, int trow, int want_a) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", shader_dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri.frag.spv", shader_dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fsz, "读不到 SPIR-V（%s）", shader_dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fs_layout;
    memset(&fs_layout, 0, sizeof fs_layout);
    fs_layout.ent = ents;
    fs_layout.nent = 2;
    nvn_gpu_layout vs_layout;
    memset(&vs_layout, 0, sizeof vs_layout);

    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &vs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz; ssrc.layout = &fs_layout;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs_sh;
    pd.st[NVN_GPU_SH_FS].sh = fs_sh;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;   // NVN_DRAW_PRIMITIVE_TRIANGLES
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "pipe_get：%d %s", r.code, e.why);
    if (!OK(r)) { nvn_gpu_shader_release(vs_sh); nvn_gpu_shader_release(fs_sh); free(vs); free(fs); return; }

    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f},
                               { 3.0f, -1.0f, 2.0f, 0.0f},
                               {-1.0f,  3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_tex tex = make_tex(2, 2, tex_fmt, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);   // nearest / clamp
    nvn_gpu_samp samp = NULL;
    r = nvn_gpu_samp_create(&sp, &samp, &e);
    CHECK(OK(r), "samp_create：%d %s", r.code, e.why);

    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);

    nvn_gpu_cmd c = begin_cmd();
    // r=NULL：整级上传，展开时按该级宽高取区域（C30 覆盖这条路径）
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, (uint64_t)trow, (uint64_t)trow * 2);

    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[0] = 0.0f; pass.color[0].clear.f[1] = 0.0f;
    pass.color[0].clear.f[2] = 0.0f; pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[0] = 0; dyn.sc[1] = 0; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = tview;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    // 保活：录制完成后、提交前就 release 视图 / 贴图 / 采样器，段完成前不能真销毁。
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    r = nvn_gpu_cmd_commit(c, 5, on_done, NULL, &e);
    CHECK(OK(r), "commit 5：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(5, 5000000000ull, &e);
    CHECK(OK(r), "wait 5：%d %s", r.code, e.why);

    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            const uint8_t *want = texels + (ty * 2 + tx) * tbpp;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2] || got[3] != want_a) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                y = 64;
                break;
            }
        }
    }
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


static void t14_textured_triangle(const char *dir) {
    static const uint8_t texels[16] = {255, 0, 0, 255,   0, 255, 0, 255,
                                       0, 0, 255, 255,   255, 255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGBA8, texels, 4, 8, 255);
}

// C30：RGB8 上传展开成 RGBA8 后经三角形采样读回；期望按 2x2 最近邻 + a=0xFF 手算。
static void t30_rgb8_sampled(const char *dir) {
    static const uint8_t texels[12] = {255, 0, 0,   0, 255, 0,
                                       0, 0, 255,   255, 255, 255};
    tri_impl(dir, NVN_FORMAT_RGB8, texels, 3, 6, 255);
}

// close 义务：在途段先 done 再返回；返回后零回调、新工作 STOPPED。
static void t10_close_inflight(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x77777777u);
    nvn_gpu_err e;
    int done_before = atomic_load(&g_ndone);   // 必须在 commit 前取：worker 可能先完成
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回前在途段没有恰好 done 一次");
    sleep_ms(50);
    CHECK(atomic_load(&g_ndone) == done_before + 1, "close 返回后仍有回调");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 真实 fence 失败路径：F 取最小、后续段 FAILED、完成值停在 F 之前。
static void t16_fence_failure(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf a = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf c = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, a, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, c, 0, 16, 0x33333333u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_fail_fence(2);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c1, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(OK(r), "commit 2：%d %s", r.code, e.why);
    r = nvn_gpu_cmd_commit(c3, 3, on_done, NULL, &e);
    CHECK(OK(r), "commit 3：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
    CHECK(OK(r), "wait(1)：%d %s", r.code, e.why);
    CHECK(nvn_gpu_fence_wait(2, 5000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(2) 应 FAILED");
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 1, "完成值应停在 1，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(3, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(3) 应 FAILED");
    const struct done_rec *d2 = wait_signal(2, base, 5000), *d3 = wait_signal(3, base, 5000);
    CHECK(d2 && d2->status == NVN_GPU_DONE_FAILED, "段 2 回调不是 FAILED");
    CHECK(d3 && d3->status == NVN_GPU_DONE_FAILED, "段 3 回调不是 FAILED");
    nvn_gpu_buf_release(a, NULL, NULL);
    nvn_gpu_buf_release(b, NULL, NULL);
    nvn_gpu_buf_release(c, NULL, NULL);
}

// C13 TIMEOUT：close(0) 在途段没完成 → TIMEOUT 且资源保留；再 close 完成，done 恰好一次。
static void t17_close_timeout(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_fill_buf(c, b, 0, 16, 0x55555555u);
    int base = atomic_load(&g_ndone);
    nvn_gpu_vk_test_delay_segment(1, 300);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    r = nvn_gpu_close(0, &e);
    CHECK(r.code == NVN_GPU_E_TIMEOUT, "close(0) 应 TIMEOUT，实为 %d", r.code);
    CHECK(atomic_load(&g_ndone) == base, "TIMEOUT 前不应有 done");
    r = nvn_gpu_close(5000000000ull, &e);
    CHECK(OK(r), "第二次 close：%d %s", r.code, e.why);
    CHECK(atomic_load(&g_ndone) == base + 1, "close 完成后 done 应恰好 +1");
    nvn_gpu_cmd probe = NULL;
    CHECK(nvn_gpu_cmd_begin(NULL, &probe, &e).code == NVN_GPU_E_STOPPED, "close 后 cmd_begin 应 STOPPED");
}


// 组合采样管线的公共搭建（C25）：FS 用 sampler2D，布局里 cls = TEX_SAMP。
static nvn_gpu_pipe make_combined_pipe(const char *dir, nvn_gpu_shader *vs_out, nvn_gpu_shader *fs_out) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_combined.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到组合采样 SPIR-V");
    if (!vs || !fs) { free(vs); free(fs); return NULL; }
    nvn_gpu_layout_ent ent;
    memset(&ent, 0, sizeof ent);
    ent.set = 0; ent.cls = NVN_GPU_RES_TEX_SAMP; ent.access = NVN_GPU_ACC_R;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = &ent; fl.nent = 1;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.stage = NVN_GPU_SH_VS; src.blob = vs; src.bytes = vsz; src.layout = &vl;
    CHECK(OK(nvn_gpu_shader_load(&src, vs_out, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    src.stage = NVN_GPU_SH_FS; src.blob = fs; src.bytes = fsz; src.layout = &fl;
    CHECK(OK(nvn_gpu_shader_load(&src, fs_out, &e)), "FS 装载：%d %s", e.rc.code, e.why);
    free(vs);
    free(fs);
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = *vs_out;
    pd.st[NVN_GPU_SH_FS].sh = *fs_out;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "pipe_get：%d %s", e.rc.code, e.why);
    return pipe;
}

static void check_triangle_pixels(nvn_gpu_buf rb, const uint8_t texels[16]) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            int tx = (int)(u * 2.0f); if (tx > 1) tx = 1;
            int ty = (int)(v * 2.0f); if (ty > 1) ty = 1;
            const uint8_t *want = texels + (ty * 2 + tx) * 4;
            const uint8_t *got = buf_cpu(rb) + (y * 64 + x) * 4;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2] || got[3] != want[3]) {
                g_fail++;
                printf("FAIL %s:%d: 三角像素 (%d,%d) = %02x%02x%02x%02x，期望 %02x%02x%02x%02x\n",
                       __FILE__, __LINE__, x, y, got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]);
                return;
            }
        }
    }
}

// C25 正例：cls = TEX_SAMP，u.ts 同时给 view 与 samp，采样结果与分开绑定一致。
static void t18_combined_sampler(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    const float verts[3][4] = {{-1.0f, -1.0f, 0.0f, 0.0f}, {3.0f, -1.0f, 2.0f, 0.0f}, {-1.0f, 3.0f, 0.0f, 2.0f}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    const uint8_t texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view tview = make_view(tex);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "samp_create：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view oview = make_view(out);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tex, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_samp_release(samp);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "commit 1：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "wait 1：%d %s", e.rc.code, e.why);
    check_triangle_pixels(rb, texels);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}

// C26：signal 违约（重复 5 / 0）→ E_ARG、F=6、各回调一次 FAILED；之后 7 也 FAILED。
static void t19_signal_contract(void) {
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_err e;
    nvn_gpu_cmd c0 = begin_cmd();
    nvn_gpu_fill_buf(c0, b, 0, 16, 0xAABBCCDDu);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c0, 5, on_done, NULL, &e);
    CHECK(OK(r), "合法 commit 5：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(5, 5000000000ull, &e)), "wait 5：%d %s", e.rc.code, e.why);
    int base = atomic_load(&g_ndone);
    nvn_gpu_cmd c1 = begin_cmd();
    nvn_gpu_fill_buf(c1, b, 0, 16, 0x11111111u);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_fill_buf(c2, b, 0, 16, 0x22222222u);
    nvn_gpu_cmd c3 = begin_cmd();
    nvn_gpu_fill_buf(c3, b, 0, 16, 0x33333333u);
    r = nvn_gpu_cmd_commit(c1, 5, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "重复 signal 5 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c2, 0, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "signal 0 应 E_ARG，实为 %d", r.code);
    r = nvn_gpu_cmd_commit(c3, 7, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_FAILED, "失败终态后 signal 7 应 E_FAILED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    CHECK(nvn_gpu_fence_done() == 5, "完成值应停在 5，实为 %llu", (unsigned long long)nvn_gpu_fence_done());
    CHECK(nvn_gpu_fence_wait(6, 2000000000ull, &e).code == NVN_GPU_E_FAILED, "wait(6) 应 FAILED");
    CHECK(wait_until(&g_ndone, base + 3, 5000), "应有 3 次 done（三次被拒段）");
    int nfail = 0, zero_seen = 0;
    for (int i = base; i < base + 3 && i < 64; i++) {
        if (g_done[i].status == NVN_GPU_DONE_FAILED) nfail++;
        if (g_done[i].signal == 0 && g_done[i].status == NVN_GPU_DONE_FAILED) zero_seen = 1;
    }
    CHECK(nfail == 3, "FAILED 回调应有 3 次，实为 %d", nfail);
    CHECK(zero_seen, "signal 0 的被拒段没有 FAILED 回调");
    uint32_t word;
    memcpy(&word, buf_cpu(b), 4);
    CHECK(word == 0xAABBCCDDu, "被拒段改写了缓冲：0x%08x", word);
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C25 反例：TEX_SAMP 缺 samp → 粘滞 E_LAYOUT，提交返回 E_LAYOUT 并进入失败终态。
static void t20_combined_missing_samp(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    nvn_gpu_pipe pipe = make_combined_pipe(dir, &vs, &fs);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view oview = make_view(out);
    nvn_gpu_tex tex = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view tview = make_view(tex);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc pass;
    memset(&pass, 0, sizeof pass);
    pass.ncolor = 1;
    pass.color[0].view = oview;
    pass.color[0].load = NVN_GPU_LOAD_CLEAR;
    pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1.0f;
    pass.w = 8; pass.h = 8; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_TEX_SAMP; b.u.ts.view = tview; b.u.ts.samp = NULL;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, &b, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_LAYOUT, "缺 samp 的 TEX_SAMP 应 E_LAYOUT，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(oview);
    nvn_gpu_tex_release(out);
    nvn_gpu_view_release(tview);
    nvn_gpu_tex_release(tex);
    nvn_gpu_shader_release(vs);
    nvn_gpu_shader_release(fs);
}



// C27：clear_view 的 is_int 必须与贴图格式一致（中立格式表驱动）。
static void t21_clear_int(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8UI, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    uint8_t zero[64];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(64, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "整数 clear commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 2000000000ull, &e)), "整数 clear wait：%d %s", e.rc.code, e.why);
    for (int i = 0; i < 16; i++) {
        uint32_t word;
        memcpy(&word, buf_cpu(b) + i * 4, 4);
        CHECK(word == 0x04030201u, "整数像素 %d = 0x%08x，期望 0x04030201", i, word);
    }
    // 反例：浮点颜色贴图却用 is_int=1，必须 E_ARG 且不执行
    nvn_gpu_tex ft = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view fv = make_view(ft);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_clear_view(c2, fv, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c2, 2, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "浮点贴图用 is_int 应 E_ARG，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 2, "F 应为 2，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(fv);
    nvn_gpu_tex_release(ft);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C27b：视图覆盖格式的 clear 明确不支持（Vulkan 按 image 格式解释 clear 值）。
static void t22_clear_view_fmt(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_VIEW_FMT);
    view_t vd;
    memset(&vd, 0, sizeof vd);
    vd.set_format = 1;
    vd.format = NVN_FORMAT_RGBA8UI;
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create(t, &vd, &v, &e);
    CHECK(OK(r), "覆盖格式视图创建：%d %s", r.code, e.why);
    nvn_gpu_cmd c = begin_cmd();
    const uint32_t vals[4] = {1, 2, 3, 4};
    nvn_gpu_clear_view(c, v, NULL, vals, 1);
    r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "覆盖格式 clear 应 E_UNSUPPORTED，实为 %d", r.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_view_release(v);
    nvn_gpu_tex_release(t);
}


// C29：Vulkan 映射按中立表重定向：展开格式必须映射到目标格式的 VkFormat（纯格式检查，不用 GPU）。
static void t29_format_expand(void) {
    for (uint32_t f = 0; f <= 0xFF; f++) {
        nvn_fmt_info i;
        if (!nvn_fmt_info_of(f, &i) || !i.expand) continue;
        VkFormat a = nvn_vk_tex_format(f), b = nvn_vk_tex_format(i.expand);
        CHECK(a == b, "0x%02X 贴图映射 %d != 目标 0x%02X 的 %d", f, (int)a, i.expand, (int)b);
    }
    // 顶点 / 存储映射不能被展开改掉
    CHECK(nvn_vk_format(0x22) == VK_FORMAT_R32G32B32_SFLOAT, "RGB32F 顶点映射被改：%d", (int)nvn_vk_format(0x22));
    CHECK(nvn_vk_format(0x19) == VK_FORMAT_R8G8B8_UNORM, "RGB8 存储映射被改：%d", (int)nvn_vk_format(0x19));
    CHECK(nvn_vk_format(0x51) == VK_FORMAT_R8_USCALED, "R8_UI2F 顶点取样映射不是 USCALED：%d", (int)nvn_vk_format(0x51));
    CHECK(nvn_vk_tex_format(0x51) == VK_FORMAT_R16_SFLOAT, "R8_UI2F 贴图映射不是 R16F：%d", (int)nvn_vk_tex_format(0x51));
    CHECK(nvn_vk_tex_format(0x19) == VK_FORMAT_R8G8B8A8_UNORM, "RGB8 贴图映射不是 RGBA8：%d", (int)nvn_vk_tex_format(0x19));
}

// C28：展开格式的回读明确不支持（不做隐式收缩）。
static void t28_rgb8_upload(void) {
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    uint8_t zero[16];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, zero);
    const uint8_t px[12] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    NVNcopyRegion r = {0, 0, 0, 2, 2, 1};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, &r, px, 6, 12);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, b, 0, 8, 16);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 6, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_UNSUPPORTED, "展开格式回读应 E_UNSUPPORTED，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 6, "F 应为 6，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(b, NULL, NULL);
}


// C31：顶点路径证据——RGB32F 位置属性不能被展开成 RGBA32F（w 会读成下一个属性的字节），
// RGBX8 也不能被展开成 RGBA8（多读 1 字节）。两者都画一个实色三角形并回读。
static void t31_vertex_formats(const char *dir) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/tri3.vert.spv", dir);
    void *vs = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/tri_solid.frag.spv", dir);
    void *fs = read_file(path, &fsz);
    CHECK(vs && fs, "读不到顶点格式用例 SPIR-V（%s）", dir);
    if (!vs || !fs) { free(vs); free(fs); return; }

    nvn_gpu_layout lay;
    memset(&lay, 0, sizeof lay);
    nvn_gpu_shader vs_sh = NULL, fs_sh = NULL;
    nvn_gpu_err e;
    nvn_gpu_shader_src ssrc;
    memset(&ssrc, 0, sizeof ssrc);
    ssrc.stage = NVN_GPU_SH_VS; ssrc.kind = NVN_GPU_BLOB_SPIRV; ssrc.entry = "main";
    ssrc.blob = vs; ssrc.bytes = vsz; ssrc.layout = &lay;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &vs_sh, &e)), "VS 装载：%d %s", e.rc.code, e.why);
    ssrc.stage = NVN_GPU_SH_FS; ssrc.blob = fs; ssrc.bytes = fsz;
    CHECK(OK(nvn_gpu_shader_load(&ssrc, &fs_sh, &e)), "FS 装载：%d %s", e.rc.code, e.why);

    const float verts32[3][4] = {{-1.0f, -1.0f, 0.0f, 99.0f},
                                 { 3.0f, -1.0f, 0.0f, 99.0f},
                                 {-1.0f,  3.0f, 0.0f, 99.0f}};
    const uint8_t vertsx[3][4] = {{0, 0, 0, 0}, {255, 0, 0, 0}, {0, 255, 0, 0}};
    nvn_gpu_buf vb32 = make_buf(sizeof verts32, NVN_GPU_MEM_SHARED, verts32);
    nvn_gpu_buf vbx = make_buf(sizeof vertsx, NVN_GPU_MEM_SHARED, vertsx);
    uint8_t zero[8 * 8 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_tex out = make_tex(8, 8, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);

    for (int which = 0; which < 2; which++) {
        nvn_gpu_pipe_desc pd;
        memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
        pd.st[NVN_GPU_SH_VS].sh = vs_sh;
        pd.st[NVN_GPU_SH_FS].sh = fs_sh;
        pd.cfmt[0] = NVN_FORMAT_RGBA8;
        pd.samples = 1;
        pd.prim = 4;
        pd.chmask.mask = 0xF;
        pd.attr[0].fmt = which ? NVN_FORMAT_RGBX8 : NVN_FORMAT_RGB32F;
        pd.attr[0].offset = 0;
        pd.attr[0].stream = 0;
        pd.stream[0].stride = which ? 4 : 16;
        nvn_gpu_pipe pipe = NULL;
        nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
        CHECK(OK(r), "顶点格式 pipe_get：%d %s", r.code, e.why);
        if (!OK(r)) continue;
        nvn_gpu_cmd c = begin_cmd();
        nvn_gpu_pass_desc pass;
        memset(&pass, 0, sizeof pass);
        pass.ncolor = 1;
        pass.color[0].view = ov;
        pass.color[0].load = NVN_GPU_LOAD_CLEAR;
        pass.color[0].store = NVN_GPU_STORE_STORE;
        pass.color[0].clear.f[3] = 1.0f;
        pass.w = 8; pass.h = 8; pass.layers = 1;
        nvn_gpu_pass_begin(c, &pass);
        nvn_gpu_set_pipe(c, pipe);
        nvn_gpu_dyn dyn;
        memset(&dyn, 0, sizeof dyn);
        dyn.nvp = 1;
        dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 8; dyn.vp[0][3] = 8; dyn.vp[0][5] = 1;
        dyn.has_sc = 1; dyn.sc[2] = 8; dyn.sc[3] = 8;
        nvn_gpu_set_dyn(c, &dyn);
        nvn_gpu_set_vbuf(c, 0, which ? vbx : vb32, 0);
        nvn_gpu_draw_args da;
        memset(&da, 0, sizeof da);
        da.prim = 4; da.count = 3; da.instances = 1;
        nvn_gpu_draw(c, &da);
        nvn_gpu_pass_end(c);
        nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 8 * 4, 8 * 8 * 4);
        r = nvn_gpu_cmd_commit(c, which + 1, on_done, NULL, &e);
        CHECK(OK(r), "顶点格式 commit %d：%d %s", which + 1, r.code, e.why);
        CHECK(OK(nvn_gpu_fence_wait(which + 1, 2000000000ull, &e)), "顶点格式 wait %d：%d %s", which + 1, e.rc.code, e.why);
        int red = 0;
        for (int i = 0; i < 8 * 8; i++) {
            const uint8_t *p = buf_cpu(rb) + i * 4;
            if (p[0] == 255 && p[1] == 0 && p[2] == 0 && p[3] == 255) red++;
        }
        if (which == 0) CHECK(red == 64, "RGB32F 顶点全屏红像素 %d / 64（w 被读成 pad）", red);
        else CHECK(red > 0, "RGBX8 顶点没有红像素（多读字节 / w 错）");
    }
    nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb32, NULL, NULL);
    nvn_gpu_buf_release(vbx, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs_sh);
    nvn_gpu_shader_release(fs_sh);
    free(vs);
    free(fs);
}


// C32：整级展开的源越界必须 E_ARG（RGB8 4×4 只给 6 字节行宽 / 12 字节总量）。
static void t32_expand_bounds(void) {
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_RGB8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    const uint8_t px[12] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, px, 6, 12);
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(rc.code == NVN_GPU_E_ARG, "展开源越界应 E_ARG，实为 %d", rc.code);
    CHECK(nvn_gpu_fence_failed() == 1, "F 应为 1，实为 %llu", (unsigned long long)nvn_gpu_fence_failed());
    nvn_gpu_tex_release(t);
}


// ---- C33–C35 公共绘制助手：只在已开始的通道里画，不管理 pass ----
static int load_shader_pair(const char *dir, const char *vs_name, const char *fs_name,
                            const nvn_gpu_layout *vlay, const nvn_gpu_layout *flay,
                            nvn_gpu_shader *vs, nvn_gpu_shader *fs, void **vsmem, void **fsmem) {
    char path[512];
    size_t vsz = 0, fsz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, vs_name);
    *vsmem = read_file(path, &vsz);
    snprintf(path, sizeof path, "%s/%s.spv", dir, fs_name);
    *fsmem = read_file(path, &fsz);
    if (!*vsmem || !*fsmem) return 0;
    nvn_gpu_layout empty;
    memset(&empty, 0, sizeof empty);
    nvn_gpu_err e;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_VS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = *vsmem; src.bytes = vsz; src.layout = vlay ? vlay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, vs, &e))) return 0;
    src.stage = NVN_GPU_SH_FS; src.blob = *fsmem; src.bytes = fsz; src.layout = flay ? flay : &empty;
    if (!OK(nvn_gpu_shader_load(&src, fs, &e))) return 0;
    return 1;
}

static void fs_draw_tex_frag(nvn_gpu_cmd c, nvn_gpu_view view, nvn_gpu_samp samp, const char *dir, const char *frag) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl;
    memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs;
    pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8;
    pd.samples = 1;
    pd.prim = 4;
    pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C33 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 0}, {3, -1, 2, 0}, {-1, 3, 0, 2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2];
    memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = view;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void fs_draw_solid(nvn_gpu_cmd c, const char *dir, const char *frag, const int sc[4]) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "tri3.vert", frag, NULL, NULL, &vs, &fs, &vsm, &fsm)) { CHECK(0, "读不到 %s 着色器", frag); return; }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C34 pipe：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1;
    dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1;
    memcpy(dyn.sc, sc, sizeof dyn.sc);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vsm); free(fsm);
}

static void expect_rect(const uint8_t *rb, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, const char *what) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            const uint8_t *p = rb + (y * 64 + x) * 4;
            if (p[0] != r || p[1] != g || p[2] != b || p[3] != 255) {
                CHECK(0, "%s 像素 (%d,%d)=%02x%02x%02x%02x，期望 %02x%02x%02xff", what, x, y, p[0], p[1], p[2], p[3], r, g, b);
                return;
            }
        }
    }
}


// C33：一个 cmd 两个通道；通道 1 清 T 红，通道 2 采样 T 画进 U，U 读回全红。
static void t33_two_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C33 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(OK(r), "C33 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(1, 5000000000ull, &e)), "C33 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 255, 0, 0, "C33 U");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C34：同一目标两通道；通道 1 左半绿，通道 2 Load 右半蓝。
static void t34_two_pass_load(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view vT = make_view(T);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int left[4] = {0, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_green.frag", left);
    nvn_gpu_pass_end(c);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    const int right[4] = {32, 0, 32, 64};
    fs_draw_solid(c, dir, "solid_blue.frag", right);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 2, on_done, NULL, &e);
    CHECK(OK(r), "C34 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(2, 5000000000ull, &e)), "C34 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 32, 64, 0, 255, 0, "C34 左半");
    expect_rect(buf_cpu(rb), 32, 0, 32, 64, 0, 0, 255, "C34 右半");
    nvn_gpu_view_release(vT); nvn_gpu_tex_release(T); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C35：三通道链；每通道后各自回读：T 红 → U 采样 T 红 → T 采样 U 反色成青。
static void t35_three_passes(const char *dir) {
    nvn_gpu_tex T = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex U = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view vT = make_view(T), vU = make_view(U);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C35 samp：%d %s", e.rc.code, e.why);
    uint8_t zero[64 * 64 * 4];
    memset(zero, 0, sizeof zero);
    nvn_gpu_buf r1 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r2 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_buf r3 = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[0] = 1.0f; p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r1, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vU; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vT, samp, dir, "tri.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, U, 0, 0, NULL, r2, 0, 64 * 4, 64 * 64 * 4);
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = vT; p.color[0].load = NVN_GPU_LOAD_LOAD; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    fs_draw_tex_frag(c, vU, samp, dir, "tri_invert.frag");
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, T, 0, 0, NULL, r3, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 3, on_done, NULL, &e);
    CHECK(OK(r), "C35 commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(3, 5000000000ull, &e)), "C35 wait：%d %s", e.rc.code, e.why);
    expect_rect(buf_cpu(r1), 0, 0, 64, 64, 255, 0, 0, "C35 通道1 T");
    expect_rect(buf_cpu(r2), 0, 0, 64, 64, 255, 0, 0, "C35 通道2 U");
    expect_rect(buf_cpu(r3), 0, 0, 64, 64, 0, 255, 255, "C35 通道3 T反色");
    nvn_gpu_view_release(vT); nvn_gpu_view_release(vU);
    nvn_gpu_tex_release(T); nvn_gpu_tex_release(U);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(r1, NULL, NULL); nvn_gpu_buf_release(r2, NULL, NULL); nvn_gpu_buf_release(r3, NULL, NULL);
}


// ---- C36–C40：索引 / 实例化 / 间接绘制 ----
static nvn_gpu_pipe color_pipe(const char *dir, int inst) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, inst ? "inst.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C36 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void run_quad(const char *dir, int inst, const void *vb, size_t vbytes,
                     nvn_gpu_buf ib, uint64_t ioff, uint32_t itype, uint32_t count, uint32_t first,
                     int32_t base_vertex, uint32_t instances, uint32_t base_instance,
                     nvn_gpu_buf ind, uint64_t ind_off, uint32_t max_draws, uint32_t stride,
                     uint8_t *rb, uint64_t sig) {
    nvn_gpu_pipe pipe = color_pipe(dir, inst);
    if (!pipe) return;
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf vbuf = make_buf(vbytes, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rbuf = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, rb);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1.0f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = count; a.first = first; a.instances = instances; a.base_instance = base_instance;
    a.ibuf = ib; a.ioff = ioff; a.itype = itype; a.base_vertex = base_vertex;
    a.ind = ind; a.ind_off = ind_off; a.max_draws = max_draws; a.stride = stride;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rbuf, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "C36 commit %llu：%d %s", (unsigned long long)sig, r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C36 wait %llu：%d %s", (unsigned long long)sig, e.rc.code, e.why);
    memcpy(rb, buf_cpu(rbuf), 64 * 64 * 4);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vbuf, NULL, NULL); nvn_gpu_buf_release(rbuf, NULL, NULL);
}

static int same_img(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 64 * 64 * 4) == 0; }
static const float quad4[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, { 1, 1, 0, 1, 0, 1, 0, 1}};
static const float blue4[4][8] = {
    {-1, -1, 0, 1, 0, 0, 1, 1}, {-1, 1, 0, 1, 0, 0, 1, 1},
    { 1, -1, 0, 1, 0, 0, 1, 1}, { 1, 1, 0, 1, 0, 0, 1, 1}};
static const float left2[4][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1},
    { 0.2f, -1, 0, 1, 1, 0, 0, 1}, { 0.2f, 1, 0, 1, 1, 0, 0, 1}};
static const float six6[6][8] = {
    {-1, -1, 0, 1, 1, 0, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, -1, 0, 1, 0, 1, 0, 1},
    { 1, -1, 0, 1, 0, 1, 0, 1}, {-1, 1, 0, 1, 1, 0, 0, 1}, {1, 1, 0, 1, 0, 1, 0, 1}};

static void c36_40(const char *dir) {
    static const uint16_t idx16[6] = {0, 1, 2, 2, 1, 3};
    static const uint16_t idxneg[10] = {0, 1, 2, 2, 1, 3, 0, 0, 0, 0};
    static const uint8_t idx8[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t ind16[5] = {6, 1, 0, 0, 0};
    static const uint32_t ind6[4] = {6, 1, 0, 0};
    nvn_gpu_buf ib16 = make_buf(sizeof idx16, NVN_GPU_MEM_SHARED, idx16);
    nvn_gpu_buf ibneg = make_buf(sizeof idxneg, NVN_GPU_MEM_SHARED, idxneg);
    nvn_gpu_buf ib8 = make_buf(sizeof idx8, NVN_GPU_MEM_SHARED, idx8);
    nvn_gpu_buf ibi = make_buf(sizeof ind16, NVN_GPU_MEM_SHARED, ind16);
    nvn_gpu_buf ini = make_buf(sizeof ind6, NVN_GPU_MEM_SHARED, ind6);
    uint8_t rb[64 * 64 * 4], ref[64 * 64 * 4], other[64 * 64 * 4];
    memset(rb, 0, sizeof rb);
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, ref, 1);
    expect_rect(ref, 0, 0, 32, 64, 255, 0, 0, "C36 左半");
    expect_rect(ref, 32, 0, 32, 64, 0, 255, 0, "C36 右半");
    // C36 负控：ioff 偏一个 u16
    run_quad(dir, 0, quad4, sizeof quad4, ibneg, 2, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 2);
    CHECK(!same_img(ref, other), "C36 负控：ioff=2 仍与期望相同（用例没在看索引）");
    // C37 base_vertex
    float vb8[8][8];
    memcpy(vb8, blue4, sizeof blue4);
    memcpy(vb8 + 4, quad4, sizeof quad4);
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 4, 1, 0, NULL, 0, 0, 0, rb, 3);
    CHECK(same_img(ref, rb), "C37 base_vertex=4 与 C36 不同");
    run_quad(dir, 0, vb8, sizeof vb8, ib16, 0, 1, 6, 0, 0, 1, 0, NULL, 0, 0, 0, other, 4);
    expect_rect(other, 0, 0, 64, 64, 0, 0, 255, "C37 base_vertex=0");
    // C37 负控：用 first=4 代替 base_vertex
    run_quad(dir, 0, vb8, sizeof vb8, ibneg, 0, 1, 6, 4, 0, 1, 0, NULL, 0, 0, 0, other, 5);
    CHECK(!same_img(ref, other), "C37 负控：first=4 仍与 base_vertex=4 相同（语义没区分）");
    // C38 u8：native 与强制展开
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 6);
    CHECK(same_img(ref, rb), "C38 u8 原生与 C36 不同");
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    run_quad(dir, 0, quad4, sizeof quad4, ib8, 0, 0, 6, 0, 0, 1, 0, NULL, 0, 0, 0, rb, 7);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(same_img(ref, rb), "C38 u8 展开与 C36 不同");
    // C39 实例化
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 2, 0, NULL, 0, 0, 0, rb, 8);
    expect_rect(rb, 0, 0, 64, 64, 255, 0, 0, "C39 两实例");
    run_quad(dir, 1, left2, sizeof left2, ib16, 0, 1, 6, 0, 0, 1, 1, NULL, 0, 0, 0, rb, 9);
    expect_rect(rb, 0, 0, 32, 64, 0, 0, 0, "C39 base_instance=1 左半清屏");
    expect_rect(rb, 32, 0, 32, 64, 255, 0, 0, "C39 base_instance=1 右半");
    // C40 间接
    run_quad(dir, 0, quad4, sizeof quad4, ib16, 0, 1, 6, 0, 0, 1, 0, ibi, 0, 1, 20, rb, 10);
    CHECK(same_img(ref, rb), "C40 索引间接与 C36 不同");
    run_quad(dir, 0, six6, sizeof six6, NULL, 0, 0, 6, 0, 0, 1, 0, ini, 0, 1, 16, rb, 11);
    CHECK(same_img(ref, rb), "C40 非索引间接与 C36 不同");
    nvn_gpu_buf_release(ib16, NULL, NULL); nvn_gpu_buf_release(ibneg, NULL, NULL); nvn_gpu_buf_release(ib8, NULL, NULL);
    nvn_gpu_buf_release(ibi, NULL, NULL); nvn_gpu_buf_release(ini, NULL, NULL);
}


// C41/C42：索引越界与 u8 间接的负例（各自独立会话，失败即终止）。
static void c41_bounds_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.ioff = 6; a.itype = 0;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C41 越界 u8 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out); nvn_gpu_buf_release(ib, NULL, NULL);
}

static void c42_u8_indirect_neg(const char *dir) {
    static const uint8_t idx[6] = {0, 1, 2, 2, 1, 3};
    static const uint32_t cmd[5] = {6, 1, 0, 0, 0};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf ind = make_buf(sizeof cmd, NVN_GPU_MEM_SHARED, cmd);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    setenv("S2A_VK_FORCE_INDEX_U8_EXPAND", "1", 1);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, color_pipe(dir, 0));
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 6; a.ibuf = ib; a.itype = 0; a.ind = ind; a.max_draws = 1; a.stride = 20;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    unsetenv("S2A_VK_FORCE_INDEX_U8_EXPAND");
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C42 u8 间接应 UNSUPPORTED，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(ind, NULL, NULL);
}


static nvn_gpu_pipe blend_pipe(const char *dir, const blend_st *b, int en, uint32_t mask) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return NULL;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = mask;
    pd.color.blend_en = en ? 1 : 0;
    if (b) pd.blend[0] = *b;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "blend pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return pipe;
}

static void blend_draw(nvn_gpu_pipe pipe, const float clear[4], const float vb[4][8], const float *bcolor, uint8_t *out, uint64_t sig) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf(sizeof *vb * 4, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    for (int i = 0; i < 4; i++) p.color[0].clear.f[i] = clear[i];
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    if (bcolor) memcpy(dyn.blend_color, bcolor, sizeof dyn.blend_color);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(r), "blend commit：%d %s", r.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "blend wait：%d %s", e.rc.code, e.why);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
}

// C69：双源混合因子（NVN 0x10–0x13 → SRC1_*）。缺 dualSrcBlend 的机型（多数 Mali / Xclipse 之外的低端档）
// 用到它的管线须按规定报 UNSUPPORTED、不崩、不建 Vulkan 管线；有特性时须正常建成。
// 先测缺特性（把已启用的位临时清掉），再测设备实况：成功的管线进后端缓存，顺序反了会命中缓存。
static nvn_gpu_rc c69_dual_src_get(const char *dir, nvn_gpu_pipe *pipe, nvn_gpu_err *e) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    *pipe = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) {
        nvn_gpu_rc bad = {NVN_GPU_E_ARG};
        return bad;
    }
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.color.blend_en = 1;
    pd.blend[0].src_rgb = 0x10; pd.blend[0].dst_rgb = 0x11;   // SRC1_COLOR / ONE_MINUS_SRC1_COLOR
    pd.blend[0].src_a = 0x2; pd.blend[0].dst_a = 0x1; pd.blend[0].eq_rgb = 1; pd.blend[0].eq_a = 1;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, pipe, e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return r;
}

static void c69_dual_src_blend(const char *dir) {
    VkBool32 has = g_dev.core_features.dualSrcBlend;
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_err e;
    memset(&e, 0, sizeof e);
    g_dev.core_features.dualSrcBlend = VK_FALSE;
    nvn_gpu_rc r = c69_dual_src_get(dir, &pipe, &e);
    g_dev.core_features.dualSrcBlend = has;
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 缺 dualSrcBlend 时双源混合管线应 UNSUPPORTED，实 %d", r.code);
    pipe = NULL;
    memset(&e, 0, sizeof e);
    r = c69_dual_src_get(dir, &pipe, &e);
    if (has) {
        CHECK(OK(r) && pipe, "C69 有 dualSrcBlend 时双源混合管线应建成：%d %s", r.code, e.why);
    } else {
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !pipe, "C69 设备无 dualSrcBlend，双源混合管线应 UNSUPPORTED，实 %d", r.code);
    }
    printf("C69 双源混合：设备 dualSrcBlend=%u，缺特性时 UNSUPPORTED%s\n", (unsigned)has,
           has ? "，有特性时建成" : "（本机实况即缺特性）");
}

static void c43_blend(const char *dir) {
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    float red[4][8] = {{-1,-1,0,1, 1,0,0,1},{-1,1,0,1, 1,0,0,1},{1,-1,0,1, 1,0,0,1},{1,1,0,1, 1,0,0,1}};
    const float blue[4] = {0, 0, 1, 1};
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x5; b.dst_rgb = 0x6; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 1);
    const uint8_t *p = out;
    CHECK((p[0] == 127 || p[0] == 128) && p[1] == 0 && (p[2] == 127 || p[2] == 128) && p[3] == 255,
          "C43 alpha 混合 = %02x%02x%02x%02x，期望 7f/80 0 7f/80 ff", p[0], p[1], p[2], p[3]);
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red, NULL, out, 2);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 加法 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    float yellow[4][8] = {{-1,-1,0,1, 1,1,0,1},{-1,1,0,1, 1,1,0,1},{1,-1,0,1, 1,1,0,1},{1,1,0,1, 1,1,0,1}};
    blend_draw(blend_pipe(dir, NULL, 0, 0x1), blue, yellow, NULL, out, 3);
    CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "C43 写掩码 = %02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
    // 负控：把 alpha 混合换成加法，像素必须不同
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x2; b.dst_rgb = 0x2; b.src_a = 0x2; b.dst_a = 0x2; b.eq_rgb = 1; b.eq_a = 1;
    blend_draw(blend_pipe(dir, &b, 1, 0xF), blue, red_half, NULL, out, 4);
    CHECK(!((out[0] == 127 || out[0] == 128) && (out[2] == 127 || out[2] == 128)), "C43 负控：加法仍等于 alpha 期望（因子没生效）");
}


static nvn_gpu_rc prim_pipe(const char *dir, uint32_t prim, nvn_gpu_pipe *out) {
    void *vsm = NULL, *fsm = NULL;
    nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, prim == 0 ? "point.vert" : "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return (nvn_gpu_rc){NVN_GPU_E_COMPILE};
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = prim; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_err e;
    nvn_gpu_rc rc = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, out, &e);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return rc;
}

static int prim_draw(const char *dir, uint32_t prim, uint32_t count, const float *vb, uint8_t *out, uint64_t sig) {
    nvn_gpu_pipe pipe = NULL;
    nvn_gpu_rc r = prim_pipe(dir, prim, &pipe);
    if (!OK(r)) return (int)r.code;
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf buf = make_buf((size_t)count * 32, NVN_GPU_MEM_SHARED, vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, buf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = prim; a.count = count; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_err e;
    r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(buf, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    return OK(r) ? 0 : (int)r.code;
}

static int count_px(const uint8_t *img, uint8_t r, uint8_t g, uint8_t b) {
    int n = 0;
    for (int i = 0; i < 64 * 64; i++) if (img[i*4] == r && img[i*4+1] == g && img[i*4+2] == b) n++;
    return n;
}

static void c44_prims(const char *dir) {
    uint8_t img[64 * 64 * 4];
    float pts[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0,0,1,1,0,0,1},{0.75f,0.75f,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 0, 3, pts[0], img, 1) == 0, "C44 点绘制失败");
    CHECK(count_px(img, 255, 0, 0) >= 3, "C44 点像素不足：%d", count_px(img, 255, 0, 0));
    float line[2][8] = {{-1,0,0,1,1,0,0,1},{1,0,0,1,1,0,0,1}};
    CHECK(prim_draw(dir, 1, 2, line[0], img, 2) == 0, "C44 线绘制失败");
    CHECK(count_px(img, 255, 0, 0) > 10, "C44 线像素不足：%d", count_px(img, 255, 0, 0));
    float strip[3][8] = {{-0.75f,-0.75f,0,1,1,0,0,1},{0,0.5f,0,1,1,0,0,1},{0.75f,-0.75f,0,1,1,0,0,1}};
    uint8_t listimg[64 * 64 * 4];
    CHECK(prim_draw(dir, 1, 3, strip[0], listimg, 3) == 0, "C44 线列表失败");
    CHECK(prim_draw(dir, 3, 3, strip[0], img, 4) == 0, "C44 线带失败");
    CHECK(memcmp(img, listimg, sizeof img) != 0, "C44 线带与线列表结果相同（拓扑没生效）");
    int strc = prim_draw(dir, 5, 4, quad4[0], img, 5); CHECK(strc == 0, "C44 三角带失败：%d", strc);
    CHECK(count_px(img, 255, 0, 0) > 500 && count_px(img, 0, 255, 0) > 500, "C44 三角带两色不足");
    nvn_gpu_pipe dummy = NULL;
    nvn_gpu_rc frc = prim_pipe(dir, 6, &dummy);
    CHECK(OK(frc), "C44 三角扇在 lavapipe 上应可用，实为 %d", frc.code);
    CHECK(prim_pipe(dir, 2, &dummy).code == NVN_GPU_E_UNSUPPORTED, "C44 线环应 UNSUPPORTED");
}


static void blend2_draw(const char *dir, const blend_st b[2], uint8_t *out0, uint8_t *out1, uint64_t sig) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad2.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C45 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;   // NVN 缺省 COPY
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.cfmt[1] = NVN_FORMAT_RGBA8;
    pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xFF;
    pd.color.blend_en = 0x3;
    pd.blend[0] = b[0]; pd.blend[1] = b[1];
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C45 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex t0 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_tex t1 = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v0 = make_view(t0), v1 = make_view(t1);
    float red_half[4][8] = {{-1,-1,0,1, 1,0,0,0.5f},{-1,1,0,1, 1,0,0,0.5f},{1,-1,0,1, 1,0,0,0.5f},{1,1,0,1, 1,0,0,0.5f}};
    nvn_gpu_buf vb = make_buf(sizeof red_half, NVN_GPU_MEM_SHARED, red_half);
    uint8_t z[64 * 64 * 4]; memset(z, 0, sizeof z);
    nvn_gpu_buf r0 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z), r1 = make_buf(sizeof z, NVN_GPU_MEM_SHARED, z);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 2; p.color[0].view = v0; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[1].view = v1; p.color[1].load = NVN_GPU_LOAD_CLEAR; p.color[1].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;
    p.color[1].clear.f[2] = 1; p.color[1].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 4; a.count = 4; a.instances = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t0, 0, 0, NULL, r0, 0, 256, 16384);
    nvn_gpu_copy_tex_buf(c, t1, 0, 0, NULL, r1, 0, 256, 16384);
    nvn_gpu_rc rc = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    CHECK(OK(rc), "C45 commit：%d %s", rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "C45 wait：%d %s", e.rc.code, e.why);
    memcpy(out0, buf_cpu(r0), 64 * 64 * 4); memcpy(out1, buf_cpu(r1), 64 * 64 * 4);
    nvn_gpu_view_release(v0); nvn_gpu_view_release(v1);
    nvn_gpu_tex_release(t0); nvn_gpu_tex_release(t1);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(r0, NULL, NULL); nvn_gpu_buf_release(r1, NULL, NULL);
}

static void c45_multi_blend(const char *dir) {
    blend_st b[2];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 2; i++) { b[i].src_rgb = 0x5; b[i].dst_rgb = 0x6; b[i].src_a = 0x2; b[i].dst_a = 0x2; b[i].eq_rgb = 1; b[i].eq_a = 1; }
    b[1].src_rgb = 0x2; b[1].dst_rgb = 0x2; b[1].src_a = 0x2; b[1].dst_a = 0x2;
    uint8_t o0[64 * 64 * 4], o1[64 * 64 * 4];
    blend2_draw(dir, b, o0, o1, 1);
    CHECK((o0[0] == 127 || o0[0] == 128) && o0[1] == 0 && (o0[2] == 127 || o0[2] == 128),
          "C45 target0 alpha = %02x%02x%02x", o0[0], o0[1], o0[2]);
    CHECK(o1[0] == 0 && o1[1] == 255 && o1[2] == 255, "C45 target1 加法 = %02x%02x%02x", o1[0], o1[1], o1[2]);
    blend_st sw[2]; sw[0] = b[1]; sw[1] = b[0];
    uint8_t n0[64 * 64 * 4], n1[64 * 64 * 4];
    blend2_draw(dir, sw, n0, n1, 2);
    CHECK(n0[0] == 255 && n0[2] == 255, "C45 负控：交换后 target0 应为加法：%02x%02x%02x", n0[0], n0[1], n0[2]);
}


static void c46_blend_const(const char *dir) {
    blend_st b;
    memset(&b, 0, sizeof b);
    b.src_rgb = 0x61; b.dst_rgb = 0x1; b.src_a = 0x61; b.dst_a = 0x1; b.eq_rgb = 1; b.eq_a = 1;
    float cb[4] = {0.25f, 0.5f, 0.75f, 0.25f};
    float clear[4] = {0, 0, 0, 1};
    float vb[4][8] = {{-1,-1,0,1,1,1,0,1},{-1,1,0,1,1,1,0,1},{1,-1,0,1,1,1,0,1},{1,1,0,1,1,1,0,1}};
    uint8_t out[64 * 64 * 4];
    blend_draw(blend_pipe(dir, &b, 1, 0xF), clear, vb, cb, out, 6);
    CHECK((out[0] == 63 || out[0] == 64) && out[1] == 0 && out[2] == 0 && (out[3] == 63 || out[3] == 64),
          "C46 混合常量 = %02x%02x%02x%02x，期望 3f/40 0 0 3f/40", out[0], out[1], out[2], out[3]);
}


static void c47_logic(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C47 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe p = NULL;
    nvn_gpu_err e;
    pd.color.logic_op = 0;   // NVN_LOGIC_OP_CLEAR
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C47 CLEAR 逻辑操作应 UNSUPPORTED，实为 %d", r.code);
    pd.color.logic_op = 3;   // 缺省 COPY
    r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &p, &e);
    CHECK(OK(r), "C47 缺省 COPY 应可建管线：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}


static void c48_fan(const char *dir) {
    uint8_t fan[64 * 64 * 4], strip[64 * 64 * 4];
    CHECK(prim_draw(dir, 6, 4, quad4[0], fan, 7) == 0, "C48 三角扇绘制失败");
    CHECK(prim_draw(dir, 5, 4, quad4[0], strip, 8) == 0, "C48 三角带绘制失败");
    CHECK(memcmp(fan, strip, sizeof fan) != 0, "C48 三角扇与三角带结果相同（拓扑映射没生效）");
    CHECK(count_px(fan, 255, 0, 0) > 500 && count_px(fan, 0, 255, 0) > 500, "C48 三角扇两色不足");
}

static void c48_mismatch(const char *dir) {
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(prim_pipe(dir, 6, &pipe)), "C48 扇管线创建失败");
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf vb = make_buf(sizeof quad4, NVN_GPU_MEM_SHARED, quad4);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 4; a.instances = 1;   // 管线是扇，draw 写带 → 必须 E_ARG
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C48 prim 与管线不一致应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vb, NULL, NULL);
}


static int restart_draw(const char *dir, int on, uint8_t *out, uint64_t sig) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    if (!load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm)) return -1;
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 5; pd.prim_restart = on ? 1 : 0; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    if (!OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e))) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); return -1; }
    // 0x10000 个顶点，0xFFFF 是合法顶点；关重启时它会产生一条连接两段的三角形
    float *vb = calloc(0x10000 * 8, sizeof(float));
    if (!vb) { nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm); return -1; }
    const float V[7][8] = {
        {-1,-1,0,1, 1,0,0,1}, {-1,1,0,1, 1,0,0,1}, {-0.95f,-1,0,1, 1,0,0,1},
        {0.8f,1,0,1, 0,1,0,1}, {1,-1,0,1, 0,1,0,1}, {1,1,0,1, 0,1,0,1},
        {0,0.5f,0,1, 1,0,0,1}};
    for (int i = 0; i < 6; i++) memcpy(vb + i * 8, V[i], sizeof V[i]);
    memcpy(vb + 0xFFFF * 8, V[6], sizeof V[6]);
    static const uint16_t idx[7] = {0,1,2,0xFFFF,3,4,5};
    nvn_gpu_buf ib = make_buf(sizeof idx, NVN_GPU_MEM_SHARED, idx);
    nvn_gpu_buf vbuf = make_buf(0x10000 * 32, NVN_GPU_MEM_SHARED, vb);
    free(vb);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, out);
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = v; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1; p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vbuf, 0);
    nvn_gpu_draw_args a;
    memset(&a, 0, sizeof a);
    a.prim = 5; a.count = 7; a.instances = 1; a.ibuf = ib; a.itype = 1;
    nvn_gpu_draw(c, &a);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 256, 16384);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e);
    if (OK(r)) (void)nvn_gpu_fence_wait(sig, 5000000000ull, &e);
    memcpy(out, buf_cpu(rb), 64 * 64 * 4);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t); nvn_gpu_buf_release(vbuf, NULL, NULL);
    nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    return OK(r) ? 0 : (int)r.code;
}

static void c49_restart(const char *dir) {
    uint8_t on[64 * 64 * 4], off[64 * 64 * 4];
    CHECK(restart_draw(dir, 1, on, 9) == 0, "C49 重启开启绘制失败");
    CHECK(restart_draw(dir, 0, off, 10) == 0, "C49 重启关闭绘制失败");
    const int px = (1 * 64 + 2) * 4;   // 关重启时 0xFFFF 顶点多出的连接三角形覆盖 (2,1)
    CHECK(on[px] == 0 && on[px + 1] == 0 && on[px + 2] == 0, "C49 开重启时指定像素应保持清屏色");
    CHECK(off[px] != 0 || off[px + 1] != 0 || off[px + 2] != 0, "C49 关重启时 0xFFFF 顶点应多画出连接三角形（覆盖指定像素）");
    CHECK(memcmp(on, off, sizeof on) != 0, "C49 重启关闭与开启结果相同（重启没生效）");
}

static void c50_list_restart(const char *dir) {
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "quad.vert", "quad.frag", NULL, NULL, &vs, &fs, &vsm, &fsm), "C50 shader");
    nvn_gpu_pipe_desc pd;
    memset(&pd, 0, sizeof pd);
    pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.prim_restart = 1; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RGBA32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RGBA32F; pd.attr[1].offset = 16; pd.attr[1].stream = 0;
    pd.stream[0].stride = 32;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
    CHECK(OK(r), "C50 列表图元带 prim_restart 应可建管线（忽略重启）：%d %s", r.code, e.why);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig);

static void c51_array(const char *dir) {
    nvn_gpu_tex src = make_tex_target(4, 4, 5, NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    static const uint8_t colors[5][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255},{255,255,0,255}};
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = make_view(src);
    view_t vd; memset(&vd, 0, sizeof vd);
    vd.set_layers = 1; vd.min_layer = 1; vd.nlayers = 4;
    nvn_gpu_err e;
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", "arr.frag", &vl, &fl, &vs, &fs, &vsm, &fsm), "C51 读着色器失败");
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C51 pipe：%d %s", e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C51 samp：%d %s", e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t l = 0; l < 5; l++) {
        uint8_t px[4*4*4];
        for (int i = 0; i < 16; i++) memcpy(px + i*4, colors[l], 4);
        nvn_gpu_tex_upload(c, src, 0, l, NULL, px, 16, 64);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, 11, on_done, NULL, &e)), "C51 commit：%d %s", e.rc.code, e.why);
    (void)0;
    CHECK(OK(nvn_gpu_fence_wait(11, 5000000000ull, &e)), "C51 wait：%d %s", e.rc.code, e.why);
    uint8_t img[64*64*4]; memcpy(img, buf_cpu(rb), sizeof img);
    const int pts[4][2] = {{16,16},{48,16},{16,48},{48,48}};
    const uint8_t *exp[4] = {colors[0], colors[1], colors[2], colors[3]};
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = img + (pts[i][1]*64 + pts[i][0])*4;
        CHECK(p[0]==exp[i][0] && p[1]==exp[i][1] && p[2]==exp[i][2], "C51 层 %d = %02x%02x%02x", i, p[0],p[1],p[2]);
    }
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
    uint8_t img2[64*64*4];
    sample_tex(dir, "arr.frag", NVN_TEXTURE_TARGET_2D_ARRAY, 5, colors[0], 5, 4, 4, &vd, img2, 12);
    const uint8_t *q = img2 + (16*64 + 16)*4;
    CHECK(q[0]==colors[1][0] && q[1]==colors[1][1] && q[2]==colors[1][2],
          "C51 负控 起始层偏 1 后第一象限应为第二层颜色，实为 %02x%02x%02x", q[0],q[1],q[2]);
}


static void sample_tex(const char *dir, const char *frag, uint32_t target, uint32_t depth,
                       const uint8_t *face_colors, uint32_t ncolors, uint32_t tw, uint32_t th,
                       const view_t *vv, uint8_t *img, uint64_t sig) {
    nvn_gpu_tex src = make_tex_target(tw, th, depth, target, NVN_FORMAT_RGBA8,
                                      NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_view sv = NULL;
    if (vv) { nvn_gpu_err ve; CHECK(OK(nvn_gpu_view_create(src, vv, &sv, &ve)), "%s 视图：%d %s", frag, ve.rc.code, ve.why); }
    else sv = make_view(src);
    nvn_gpu_layout_ent ents[2]; memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R; ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R; ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl; memset(&fl, 0, sizeof fl); fl.ent = ents; fl.nent = 2;
    nvn_gpu_layout vl; memset(&vl, 0, sizeof vl);
    void *vsm = NULL, *fsm = NULL; nvn_gpu_shader vs = NULL, fs = NULL;
    CHECK(load_shader_pair(dir, "tri.vert", frag, &vl, &fl, &vs, &fs, &vsm, &fsm), "%s 读着色器失败", frag);
    nvn_gpu_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.color.logic_op = 0x3;
    pd.st[NVN_GPU_SH_VS].sh = vs; pd.st[NVN_GPU_SH_FS].sh = fs;
    pd.cfmt[0] = NVN_FORMAT_RGBA8; pd.samples = 1; pd.prim = 4; pd.chmask.mask = 0xF;
    pd.attr[0].fmt = NVN_FORMAT_RG32F; pd.attr[0].offset = 0; pd.attr[0].stream = 0;
    pd.attr[1].fmt = NVN_FORMAT_RG32F; pd.attr[1].offset = 8; pd.attr[1].stream = 0;
    pd.stream[0].stride = 16;
    nvn_gpu_pipe pipe = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "%s pipe：%d %s", frag, e.rc.code, e.why);
    samp_par sp; memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "%s samp：%d %s", frag, e.rc.code, e.why);
    const float verts[3][4] = {{-1,-1,0,0},{3,-1,2,0},{-1,3,0,2}};
    nvn_gpu_buf vb = make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
    uint8_t zero[64*64*4]; memset(zero,0,sizeof zero);
    nvn_gpu_buf rb = make_buf(sizeof zero, NVN_GPU_MEM_SHARED, zero);
    nvn_gpu_cmd c = begin_cmd();
    for (uint32_t f = 0; f < ncolors; f++) {
        uint8_t px[64*4];
        for (uint32_t i = 0; i < tw * th; i++) memcpy(px + i*4, face_colors + f*4, 4);
        nvn_gpu_tex_upload(c, src, 0, f, NULL, px, tw*4, tw*4*th);
    }
    nvn_gpu_pass_desc pass; memset(&pass, 0, sizeof pass);
    pass.ncolor = 1; pass.color[0].view = ov; pass.color[0].load = NVN_GPU_LOAD_CLEAR; pass.color[0].store = NVN_GPU_STORE_STORE;
    pass.color[0].clear.f[3] = 1; pass.w = 64; pass.h = 64; pass.layers = 1;
    nvn_gpu_pass_begin(c, &pass);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn; memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1; dyn.vp[0][2] = 64; dyn.vp[0][3] = 64; dyn.vp[0][5] = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding binds[2]; memset(binds, 0, sizeof binds);
    binds[0].ent = 0; binds[0].cls = NVN_GPU_RES_TEX; binds[0].u.view = sv;
    binds[1].ent = 1; binds[1].cls = NVN_GPU_RES_SAMP; binds[1].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, binds, 2);
    nvn_gpu_draw_args da; memset(&da, 0, sizeof da); da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 256, 16384);
    CHECK(OK(nvn_gpu_cmd_commit(c, sig, on_done, NULL, &e)), "%s commit：%d %s", frag, e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(sig, 5000000000ull, &e)), "%s wait：%d %s", frag, e.rc.code, e.why);
    memcpy(img, buf_cpu(rb), 64*64*4);
    nvn_gpu_view_release(sv); nvn_gpu_view_release(ov);
    nvn_gpu_tex_release(src); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL); nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs); free(vsm); free(fsm);
}

static void check_cell(const char *tag, const uint8_t *img, int x, int y, const uint8_t *exp) {
    const uint8_t *p = img + (y*64 + x)*4;
    CHECK(p[0]==exp[0] && p[1]==exp[1] && p[2]==exp[2], "%s (%d,%d)=%02x%02x%02x 期望 %02x%02x%02x",
          tag, x, y, p[0],p[1],p[2], exp[0],exp[1],exp[2]);
}

static void c52_cube(const char *dir) {
    static const uint8_t c[6][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255},{255,0,255,255},{0,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, c[0], 6, 1, 1, NULL, img, 13);
    for (int f = 0; f < 6; f++) check_cell("C52", img, 10 + (f%3)*22, 16 + (f/3)*32, c[f]);
    uint8_t sw[6][4];
    memcpy(sw, c, sizeof sw);
    memcpy(sw[0], c[1], 4); memcpy(sw[1], c[0], 4);
    uint8_t img2[64*64*4];
    sample_tex(dir, "cube.frag", NVN_TEXTURE_TARGET_CUBEMAP, 6, sw[0], 6, 1, 1, NULL, img2, 14);
    check_cell("C52 负控", img2, 10, 16, c[1]);
    CHECK(img2[0] != img[0] || img2[1] != img[1] || img2[2] != img[2], "C52 换面顺序后 (+X) 应不同");
}

static void c53_cube_array(const char *dir) {
    uint8_t c[12][4];
    memset(c, 0, sizeof c);
    for (int f = 0; f < 6; f++) { c[f][0] = 255; }          // 第 0 个立方体 +X 红、其余面随 f 变化
    for (int f = 0; f < 6; f++) { c[6+f][1] = 255; c[6+f][2] = 255; }   // 第 1 个立方体全青
    c[0][0] = 255; c[0][1] = 0; c[0][2] = 0;
    uint8_t img[64*64*4];
    sample_tex(dir, "cubearr.frag", NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, 12, c[0], 12, 1, 1, NULL, img, 15);
    const uint8_t red[4] = {255,0,0,255}, cyan[4] = {0,255,255,255};
    check_cell("C53", img, 16, 32, red);
    check_cell("C53", img, 48, 32, cyan);
}

static void c54_3d(const char *dir) {
    static const uint8_t c[4][4] = {{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,255,255}};
    uint8_t img[64*64*4];
    sample_tex(dir, "tex3d.frag", NVN_TEXTURE_TARGET_3D, 4, c[0], 4, 2, 2, NULL, img, 16);
    check_cell("C54", img, 16, 16, c[0]);
    check_cell("C54", img, 48, 16, c[1]);
    check_cell("C54", img, 16, 48, c[2]);
    check_cell("C54", img, 48, 48, c[3]);
}


static void c55_compute(const char *dir) {
    char path[512]; size_t csz = 0;
    snprintf(path, sizeof path, "%s/comp.comp.spv", dir);
    void *cm = read_file(path, &csz);
    CHECK(cm != NULL, "C55 读不到 comp.comp.spv");
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1; ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl; memset(&cl, 0, sizeof cl); cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader_src src; memset(&src, 0, sizeof src);
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main"; src.blob = cm; src.bytes = csz; src.layout = &cl;
    nvn_gpu_shader sh = NULL; nvn_gpu_err e;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "C55 shader：%d %s", e.rc.code, e.why);
    nvn_gpu_stage_desc cs; memset(&cs, 0, sizeof cs); cs.sh = sh;
    nvn_gpu_pipe cp = NULL;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e)), "C55 cpipe：%d %s", e.rc.code, e.why);
    uint32_t data[64]; memset(data, 0, sizeof data);
    nvn_gpu_buf buf = make_buf(sizeof data, NVN_GPU_MEM_SHARED, data);
    nvn_gpu_binding b; memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO;
    b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = sizeof data;
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 4, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c, 17, on_done, NULL, &e)), "C55 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(17, 5000000000ull, &e)), "C55 wait：%d %s", e.rc.code, e.why);
    uint32_t rd[64]; memcpy(rd, buf_cpu(buf), sizeof rd);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 dispatch 结果错 %d 项（首项 %u）", bad, rd[0]);
    // 间接 dispatch：先把 GPU 缓冲清 0，否则即使什么都不做也会读到上一次的 1..64
    memset(buf_cpu(buf), 0, sizeof rd);
    uint32_t indv[3] = {4, 1, 1};
    nvn_gpu_buf ind = make_buf(sizeof indv, NVN_GPU_MEM_SHARED, indv);
    nvn_gpu_cmd c2 = begin_cmd();
    nvn_gpu_set_pipe(c2, cp);
    nvn_gpu_bind(c2, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c2, cp, 0, 0, 0, ind, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c2, 18, on_done, NULL, &e)), "C55 间接 commit：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(18, 5000000000ull, &e)), "C55 间接 wait：%d %s", e.rc.code, e.why);
    memcpy(rd, buf_cpu(buf), sizeof rd);
    bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != i + 1) bad++;
    CHECK(bad == 0, "C55 间接 dispatch 结果错 %d 项", bad);
    // 0 组 dispatch：空操作，命令缓冲仍应可提交
    nvn_gpu_cmd c4 = begin_cmd();
    nvn_gpu_set_pipe(c4, cp);
    nvn_gpu_bind(c4, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c4, cp, 0, 1, 1, NULL, 0);
    CHECK(OK(nvn_gpu_cmd_commit(c4, 20, on_done, NULL, &e)), "C55 0 组 dispatch 后 commit 失败：%d %s", e.rc.code, e.why);
    CHECK(OK(nvn_gpu_fence_wait(20, 5000000000ull, &e)), "C55 0 组 wait：%d %s", e.rc.code, e.why);
    nvn_gpu_buf_release(ind, NULL, NULL);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh); free(cm);
}

// ---------- C56–C68：D8 补口（资源类别 + push 常量）契约用例 + D9 静默错复现 ----------
// 期望值全部按契约与 GLSL/SPIR-V 语义手算，不经过被测实现的路径。

static nvn_gpu_shader load_one_shader(const char *dir, const char *name, nvn_gpu_stage stage,
                                      const nvn_gpu_layout *lay) {
    char path[512];
    size_t sz = 0;
    snprintf(path, sizeof path, "%s/%s.spv", dir, name);
    void *mem = read_file(path, &sz);
    CHECK(mem != NULL, "读不到 %s.spv", name);
    if (!mem) return NULL;
    nvn_gpu_shader_src src;
    memset(&src, 0, sizeof src);
    src.stage = stage; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = mem; src.bytes = sz; src.layout = lay;
    nvn_gpu_err e;
    nvn_gpu_shader sh = NULL;
    CHECK(OK(nvn_gpu_shader_load(&src, &sh, &e)), "%s shader：%d %s", name, e.rc.code, e.why);
    free(mem);
    return sh;
}

static nvn_gpu_pipe mk_cpipe(const char *dir, const char *name, const nvn_gpu_layout *lay, nvn_gpu_shader *sh_out) {
    *sh_out = NULL;
    nvn_gpu_shader sh = load_one_shader(dir, name, NVN_GPU_SH_CS, lay);
    if (!sh) return NULL;
    nvn_gpu_stage_desc cs;
    memset(&cs, 0, sizeof cs);
    cs.sh = sh;
    nvn_gpu_err e;
    nvn_gpu_pipe cp = NULL;
    CHECK(OK(nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e)), "%s cpipe：%d %s", name, e.rc.code, e.why);
    *sh_out = sh;
    return cp;
}

static void fill_gpipe_desc(nvn_gpu_pipe_desc *pd, nvn_gpu_shader vs, nvn_gpu_shader fs, uint32_t dfmt) {
    memset(pd, 0, sizeof *pd);
    pd->color.logic_op = 0x3;
    pd->st[NVN_GPU_SH_VS].sh = vs;
    pd->st[NVN_GPU_SH_FS].sh = fs;
    pd->cfmt[0] = NVN_FORMAT_RGBA8;
    pd->dfmt = dfmt;
    pd->samples = 1;
    pd->prim = 4;
    pd->chmask.mask = 0xF;
    pd->attr[0].fmt = NVN_FORMAT_RGBA32F; pd->attr[0].offset = 0; pd->attr[0].stream = 0;
    pd->stream[0].stride = 16;
}

static void full_dyn(nvn_gpu_dyn *d) {
    memset(d, 0, sizeof *d);
    d->nvp = 1; d->vp[0][2] = 64; d->vp[0][3] = 64; d->vp[0][5] = 1;
    d->has_sc = 1; d->sc[2] = 64; d->sc[3] = 64;
}

static nvn_gpu_buf full_vb(void) {
    const float verts[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
    return make_buf(sizeof verts, NVN_GPU_MEM_SHARED, verts);
}

// C56：FS push 常量。两次 push 两个颜色，两笔各画一半屏。
// 手算：RGBA8 的 UNORM 量化 0.25*255=63.75→64、0.5*255=127.5→128、0.75*255=191.25→191。
static void c56_push_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C56 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.sc[3] = 32;   // 上半屏第一笔
    const float ca[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, ca, sizeof ca);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    const float cb[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, cb, sizeof cb);
    dyn.sc[1] = 32;   // 下半屏第二笔
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 100, on_done, NULL, &e);
    CHECK(OK(r), "C56 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(100, 5000000000ull, &e);
    CHECK(OK(r), "C56 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 64, 128, 191, "C56 上半");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 255, 0, 0, "C56 下半");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C57：CS push 常量 + SSBO 绑定共存。d[i] = 7 + i（i = 0..63 → 7..70）。
static void c57_push_cs(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "pushc.comp", &cl, &sh);
    CHECK(cp != NULL, "C57 cpipe 为空");
    if (!cp) return;
    nvn_gpu_buf buf = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64 * 4;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    uint32_t v = 7;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 0, &v, sizeof v);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 101, on_done, NULL, &e);
    CHECK(OK(r), "C57 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(101, 5000000000ull, &e);
    CHECK(OK(r), "C57 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(buf);
    int bad = 0;
    for (uint32_t i = 0; i < 64; i++) if (rd[i] != 7 + i) bad++;
    CHECK(bad == 0, "C57 push 结果错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C58d：同一个 cmd 切到更大 push 范围时，旧阶段值必须保留且写入不越界。
static void c58d_push_pipe_resize(const char *dir) {
    nvn_gpu_layout_ent ent = {0};
    ent.set = 0; ent.cls = NVN_GPU_RES_SSBO; ent.access = NVN_GPU_ACC_W;
    ent.binding = 0; ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout small = {0}, large = {0};
    small.ent = large.ent = &ent; small.nent = large.nent = 1;
    small.push_bytes = 4; large.push_bytes = 16;
    nvn_gpu_shader a = NULL, b = NULL;
    nvn_gpu_pipe pa = mk_cpipe(dir, "pushc.comp", &small, &a);
    nvn_gpu_pipe pb = mk_cpipe(dir, "pushc.comp", &large, &b);
    CHECK(pa && pb, "C58d 两个计算管线必须可创建");
    if (!pa || !pb) { if (a) nvn_gpu_shader_release(a); if (b) nvn_gpu_shader_release(b); return; }
    nvn_gpu_buf out = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd cmd = begin_cmd();
    nvn_gpu_set_pipe(cmd, pa);
    uint32_t value = 7, last = 42;
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 0, &value, sizeof value);
    nvn_gpu_set_pipe(cmd, pb);
    uint32_t capacity = nvn_gpu_vk_test_push_capacity(cmd, NVN_GPU_SH_CS);
    CHECK(capacity >= large.push_bytes, "C58d 切换管线后的容量 %u，小于新范围 %u", capacity, large.push_bytes);
    if (capacity < large.push_bytes) {
        nvn_gpu_cmd_abort(cmd); nvn_gpu_buf_release(out, NULL, NULL);
        nvn_gpu_shader_release(a); nvn_gpu_shader_release(b); return;
    }
    nvn_gpu_push(cmd, NVN_GPU_SH_CS, 12, &last, sizeof last);
    nvn_gpu_binding binding = {0}; binding.ent = 0; binding.cls = NVN_GPU_RES_SSBO;
    binding.u.b.buf = out; binding.u.b.len = 64 * 4;
    nvn_gpu_bind(cmd, NVN_GPU_SH_CS, &binding, 1);
    nvn_gpu_dispatch(cmd, pb, 1, 1, 1, NULL, 0);
    nvn_gpu_err error; nvn_gpu_rc result = nvn_gpu_cmd_commit(cmd, 102, on_done, NULL, &error);
    CHECK(OK(result), "C58d commit %d %s", result.code, error.why);
    result = nvn_gpu_fence_wait(102, 5000000000ull, &error);
    CHECK(OK(result), "C58d wait %d %s", result.code, error.why);
    const uint32_t *values = (const uint32_t *)buf_cpu(out);
    int bad = 0; for (uint32_t i = 0; i < 64; i++) if (values[i] != 7 + i) bad++;
    CHECK(!bad, "C58d 切管线后 push 值错误：%d 项", bad);
    nvn_gpu_buf_release(out, NULL, NULL); nvn_gpu_shader_release(a); nvn_gpu_shader_release(b);
}

// C58a–c：push 越界 / 未声明阶段 → 粘滞 E_ARG（契约：只许写进该阶段声明范围）。
// 失败段让时间线进终态、恢复只能关会话重开，故三个子例各占独立会话（main 里关/开）。
static nvn_gpu_pipe c58_cpipe(const char *dir, nvn_gpu_shader *sh_out) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1; cl.push_off = 0; cl.push_bytes = 4;
    return mk_cpipe(dir, "pushc.comp", &cl, sh_out);
}

static void c58a_push_oob_cs(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58a cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_CS, 2, &v, 4);   // 声明 [0,4)：写 [2,4) → off+4=6 > 4
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 102, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58a 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58b_push_bad_stage(const char *dir) {
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = c58_cpipe(dir, &sh);
    CHECK(cp != NULL, "C58b cpipe 为空");
    if (!cp) { nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    uint32_t v = 1;
    nvn_gpu_push(c, NVN_GPU_SH_FS, 0, &v, 4);   // 计算管线无 FS 阶段 → 未声明范围
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 103, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58b 未声明阶段 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_shader_release(sh);
}

static void c58c_push_oob_fs(const char *dir) {
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.push_off = 0; fl.push_bytes = 16;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "pushc.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 pushc 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C58c pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_DONTCARE;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    const float part[2] = {0.1f, 0.2f};
    nvn_gpu_push(c, NVN_GPU_SH_FS, 4, part, sizeof part);   // [4,8) ⊆ [0,16)：合法，不应置错
    nvn_gpu_push(c, NVN_GPU_SH_FS, 12, part, sizeof part);  // [12,8)：12+8=20 > 16 → E_ARG
    nvn_gpu_pass_end(c);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 104, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C58c 越界 push 应 E_ARG，实为 %d", r.code);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C59a：缓冲视图显式携带格式，后端必须能创建并释放它。
static void c59a_buffer_view(void) {
    nvn_gpu_buf b = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view v = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
    CHECK(OK(r) && v != NULL, "C59a R32UI 缓冲视图应创建成功：%d %s", r.code, e.why);
    if (v) nvn_gpu_view_release(v);
    for (int i = 0; i < 2; i++) {
        g_texel_feature_mask = i ? VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT
                                 : VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
        v = NULL;
        r = nvn_gpu_view_create_buf(b, 0, 16, NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !v,
              "C59a 模拟缺一种 texel 特性时不可创建双 usage 视图（%d）：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    g_texel_feature_mask = 0;
    const uint64_t offsets[] = {0, 0, 16};
    const uint64_t lengths[] = {0, 15, 4};
    for (int i = 0; i < 3; i++) {
        v = NULL;
        r = nvn_gpu_view_create_buf(b, offsets[i], lengths[i], NVN_FORMAT_R32UI, &v, &e);
        CHECK(r.code == NVN_GPU_E_ARG && !v, "C59a 非法范围 %d 应返回 ARG：%d", i, r.code);
        if (v) nvn_gpu_view_release(v);
    }
    nvn_gpu_buf_release(b, NULL, NULL);
}

// C59：Unknown 采样纹素缓冲的格式来自显式 view，读取结果须与输入一致。
static void c59_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2] = {0};
    ents[0].cls = NVN_GPU_RES_TBUF; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ents; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59 Unknown 采样 TBUF 管线必须创建成功");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    const uint32_t src[4] = {11, 22, 33, 44};
    nvn_gpu_buf input = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_buf output = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_view view = NULL; nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_view_create_buf(input, 0, sizeof src, NVN_FORMAT_R32UI, &view, &e);
    CHECK(OK(r) && view, "C59 视图创建失败：%d %s", r.code, e.why);
    if (!view) { nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b[2] = {0};
    b[0].ent = 0; b[0].cls = NVN_GPU_RES_TBUF; b[0].u.view = view;
    b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
    int retired_before = atomic_load(&g_retired);
    nvn_gpu_buf_release(input, on_retire, NULL);
    nvn_gpu_view_release(view);
    CHECK(atomic_load(&g_retired) == retired_before, "C59 录制中的视图未保活底层缓冲");
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    r = nvn_gpu_cmd_commit(c, 105, on_done, NULL, &e);
    CHECK(OK(r), "C59 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(105, 5000000000ull, &e);
    CHECK(OK(r), "C59 wait：%d %s", r.code, e.why);
    CHECK(!memcmp(buf_cpu(output), src, sizeof src), "C59 采样读回与输入不同");
    CHECK(wait_until(&g_retired, retired_before + 1, 2000), "C59 完成后底层缓冲未退休");
    CHECK(atomic_load(&g_retired) == retired_before + 1, "C59 底层缓冲退休次数不为一");
    nvn_gpu_buf_release(output, NULL, NULL);

    // 同一 Unknown 着色器用 RGBA8UI 时，读取每个 texel 的首分量。
    const uint8_t packed[16] = {1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16};
    const uint32_t expected[4] = {1,5,9,13};
    input = make_buf(sizeof packed, NVN_GPU_MEM_SHARED, packed);
    output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
    view = NULL;
    r = nvn_gpu_view_create_buf(input, 0, sizeof packed, NVN_FORMAT_RGBA8UI, &view, &e);
    CHECK(OK(r) && view, "C59 RGBA8UI 视图创建失败：%d %s", r.code, e.why);
    if (view) {
        c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
        b[0].u.view = view; b[1].u.b.buf = output;
        nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2);
        nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
        r = nvn_gpu_cmd_commit(c, 106, on_done, NULL, &e);
        CHECK(OK(r), "C59 RGBA8UI commit：%d %s", r.code, e.why);
        r = nvn_gpu_fence_wait(106, 5000000000ull, &e);
        CHECK(OK(r), "C59 RGBA8UI wait：%d %s", r.code, e.why);
        CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C59 RGBA8UI 采样结果不符");
        nvn_gpu_view_release(view);
    }
    nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// 失败提交令时间线终止，旧绑定负测必须独占会话。
static void c59b_legacy_binding(const char *dir) {
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tbuf.comp", &lay, &sh);
    CHECK(cp != NULL, "C59b 管线创建失败");
    if (!cp) { if (sh) nvn_gpu_shader_release(sh); return; }
    nvn_gpu_buf input = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding legacy = {0}; legacy.cls = NVN_GPU_RES_TBUF;
    legacy.u.b.buf = input; legacy.u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &legacy, 1);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C59b 旧 u.b 应返回 UNSUPPORTED：%d %s", r.code, e.why);
    nvn_gpu_buf_release(input, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C60：IMAGE_TBUF（storage texel buffer）。imageStore 写 src[i]+100 → {101,102,103,104}。
static void c60_image_tbuf(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_IMAGE_TBUF; ents[0].access = NVN_GPU_ACC_W;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "itbuf.comp", &cl, &sh);
    CHECK(cp != NULL, "C60 cpipe 为空");
    if (!cp) return;
    const uint32_t src[4] = {1, 2, 3, 4};
    nvn_gpu_buf ib = make_buf(sizeof src, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf sb = make_buf(sizeof src, NVN_GPU_MEM_SHARED, src);
    nvn_gpu_view iv = NULL;
    nvn_gpu_err e;
    nvn_gpu_rc vr = nvn_gpu_view_create_buf(ib, 0, sizeof src, NVN_FORMAT_R32UI, &iv, &e);
    CHECK(OK(vr) && iv, "C60 视图创建失败：%d %s", vr.code, e.why);
    if (!iv) { nvn_gpu_buf_release(ib, NULL, NULL); nvn_gpu_buf_release(sb, NULL, NULL); nvn_gpu_shader_release(sh); return; }
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_IMAGE_TBUF; bs[0].u.view = iv;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = sb; bs[1].u.b.off = 0; bs[1].u.b.len = sizeof src;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 107, on_done, NULL, &e);
    CHECK(OK(r), "C60 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(107, 5000000000ull, &e);
    CHECK(OK(r), "C60 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(ib);
    int bad = 0;
    for (int i = 0; i < 4; i++) if (rd[i] != src[i] + 100u) bad++;
    CHECK(bad == 0, "C60 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(iv);
    nvn_gpu_buf_release(ib, NULL, NULL);
    nvn_gpu_buf_release(sb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C60b：Unknown 存储视图须按已启用的 without-format 写特性建管线。
static void c60b_unknown_image_tbuf(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageWriteWithoutFormat;
    CHECK(enabled, "C60b 宿主未启用 shaderStorageImageWriteWithoutFormat，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent = {0};
    ent.cls = NVN_GPU_RES_IMAGE_TBUF; ent.access = NVN_GPU_ACC_W;
    ent.count = 1; ent.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = &ent; lay.nent = 1;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60b shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 缺特性时 shader 仍须装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh;
        nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60b 缺写特性管线应 UNSUPPORTED，实 %d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageWriteWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60b 有特性时 shader 应装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60b 有写特性管线应成功：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {5, 6, 7, 8};
            nvn_gpu_buf b = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(b, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60b 视图创建失败：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding bind = {0}; bind.cls = NVN_GPU_RES_IMAGE_TBUF; bind.u.view = v;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, &bind, 1);
                nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 1, on_done, NULL, &e);
                CHECK(OK(r), "C60b commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(1, 5000000000ull, &e);
                CHECK(OK(r), "C60b wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(b), expected, sizeof expected), "C60b 写回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(b, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C60c：Unknown 存储读取独立检查 read-without-format 特性并执行读回。
static void c60c_unknown_image_tbuf_read(const char *dir) {
    VkBool32 enabled = g_dev.core_features.shaderStorageImageReadWithoutFormat;
    CHECK(enabled, "C60c 宿主未启用 read-without-format，无法验证正控");
    if (!enabled) return;
    nvn_gpu_layout_ent ent[2] = {0};
    ent[0].cls = NVN_GPU_RES_IMAGE_TBUF; ent[0].access = NVN_GPU_ACC_R;
    ent[0].count = 1; ent[0].stages = 1u << NVN_GPU_SH_CS;
    ent[1].cls = NVN_GPU_RES_SSBO; ent[1].access = NVN_GPU_ACC_W;
    ent[1].binding = 1; ent[1].count = 1; ent[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout lay = {0}; lay.ent = ent; lay.nent = 2;
    char path[512]; size_t bytes = 0;
    snprintf(path, sizeof path, "%s/itbuf_unknown_read.comp.spv", dir);
    void *blob = read_file(path, &bytes);
    CHECK(blob != NULL, "C60c shader 文件缺失");
    if (!blob) return;
    nvn_gpu_shader_src src = {0};
    src.stage = NVN_GPU_SH_CS; src.kind = NVN_GPU_BLOB_SPIRV; src.entry = "main";
    src.blob = blob; src.bytes = bytes; src.layout = &lay;
    nvn_gpu_err e; nvn_gpu_shader sh = NULL;
    g_dev.core_features.shaderStorageImageReadWithoutFormat = VK_FALSE;
    nvn_gpu_rc r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 缺特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(r.code == NVN_GPU_E_UNSUPPORTED && !cp, "C60c 缺读特性应 UNSUPPORTED：%d", r.code);
        nvn_gpu_shader_release(sh);
    }
    g_dev.core_features.shaderStorageImageReadWithoutFormat = enabled;
    sh = NULL; r = nvn_gpu_shader_load(&src, &sh, &e);
    CHECK(OK(r) && sh, "C60c 有特性时 shader 装载：%d %s", r.code, e.why);
    if (sh) {
        nvn_gpu_stage_desc cs = {0}; cs.sh = sh; nvn_gpu_pipe cp = NULL;
        r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
        CHECK(OK(r) && cp, "C60c 管线：%d %s", r.code, e.why);
        if (cp) {
            const uint32_t expected[4] = {19, 23, 29, 31};
            nvn_gpu_buf input = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, expected);
            nvn_gpu_buf output = make_buf(sizeof expected, NVN_GPU_MEM_SHARED, NULL);
            nvn_gpu_view v = NULL;
            r = nvn_gpu_view_create_buf(input, 0, sizeof expected, NVN_FORMAT_R32UI, &v, &e);
            CHECK(OK(r) && v, "C60c 视图：%d %s", r.code, e.why);
            if (v) {
                nvn_gpu_cmd c = begin_cmd(); nvn_gpu_set_pipe(c, cp);
                nvn_gpu_binding b[2] = {0};
                b[0].cls = NVN_GPU_RES_IMAGE_TBUF; b[0].u.view = v;
                b[1].ent = 1; b[1].cls = NVN_GPU_RES_SSBO; b[1].u.b.buf = output; b[1].u.b.len = sizeof expected;
                nvn_gpu_bind(c, NVN_GPU_SH_CS, b, 2); nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
                r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
                CHECK(OK(r), "C60c commit：%d %s", r.code, e.why);
                r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
                CHECK(OK(r), "C60c wait：%d %s", r.code, e.why);
                CHECK(!memcmp(buf_cpu(output), expected, sizeof expected), "C60c 读回不符");
                nvn_gpu_view_release(v);
            }
            nvn_gpu_buf_release(input, NULL, NULL); nvn_gpu_buf_release(output, NULL, NULL);
        }
        nvn_gpu_shader_release(sh);
    }
    free(blob);
}

// C61：IMAGE（storage image）。uimage2D r32ui 4x4 imageStore(x,y)=x+4y+1 → 1..16。
static void c61_storage_image(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_IMAGE; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "img.comp", &cl, &sh);
    CHECK(cp != NULL, "C61 cpipe 为空");
    if (!cp) return;
    nvn_gpu_tex t = make_tex(4, 4, NVN_FORMAT_R32UI, NVN_GPU_TEX_STORAGE | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_buf rb = make_buf(16 * 4, NVN_GPU_MEM_SHARED, NULL);
    const uint32_t zeros[16] = {0};
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, zeros, 16, 64);   // 上传后贴图落到 GENERAL，可进存储用途
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_IMAGE; b.u.view = v;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_copy_tex_buf(c, t, 0, 0, NULL, rb, 0, 16, 64);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 108, on_done, NULL, &e);
    CHECK(OK(r), "C61 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(108, 5000000000ull, &e);
    CHECK(OK(r), "C61 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(rb);
    int bad = 0;
    for (int i = 0; i < 16; i++) if (rd[i] != (uint32_t)i + 1) bad++;
    CHECK(bad == 0, "C61 imageStore 读回错 %d 项（首项 %u）", bad, rd[0]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C62：TEX_SAMP 组合采样项走计算路径。texelFetch(1,0) = 红 {1,0,0,1} → 位模式 {3F800000,0,0,3F800000}。
static void c62_tsamp_cs(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX_SAMP; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 1; ents[0].stages = 1u << NVN_GPU_SH_CS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SSBO; ents[1].access = NVN_GPU_ACC_W;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = ents; cl.nent = 2;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "tsamp.comp", &cl, &sh);
    CHECK(cp != NULL, "C62 cpipe 为空");
    if (!cp) return;
    static const uint8_t texels[16] = {0, 0, 0, 255,  255, 0, 0, 255,
                                       0, 0, 0, 255,  0, 0, 0, 255};
    nvn_gpu_tex t = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view v = make_view(t);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_err e;
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C62 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_buf out = make_buf(16, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, t, 0, 0, NULL, texels, 8, 16);
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding bs[2];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX_SAMP; bs[0].u.ts.view = v; bs[0].u.ts.samp = samp;
    bs[1].ent = 1; bs[1].cls = NVN_GPU_RES_SSBO; bs[1].u.b.buf = out; bs[1].u.b.off = 0; bs[1].u.b.len = 16;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, bs, 2);
    nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 109, on_done, NULL, &e);
    CHECK(OK(r), "C62 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(109, 5000000000ull, &e);
    CHECK(OK(r), "C62 wait：%d %s", r.code, e.why);
    const uint32_t *rd = (const uint32_t *)buf_cpu(out);
    CHECK(rd[0] == 0x3F800000u && rd[1] == 0 && rd[2] == 0 && rd[3] == 0x3F800000u,
          "C62 texelFetch = %08x %08x %08x %08x，期望 3f800000 0 0 3f800000", rd[0], rd[1], rd[2], rd[3]);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
    nvn_gpu_samp_release(samp);
    nvn_gpu_buf_release(out, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C63：描述符数组元素位。t[2] 绑定 elem0=红、elem1=绿，着色器取 t[1] → 全绿。
// 若后端把 elem 当 0 写，elem1 会盖到元素 0、t[1] 留空——本例必红。
static void c63_desc_array_elem(const char *dir) {
    nvn_gpu_layout_ent ents[2];
    memset(ents, 0, sizeof ents);
    ents[0].set = 0; ents[0].cls = NVN_GPU_RES_TEX; ents[0].access = NVN_GPU_ACC_R;
    ents[0].binding = 0; ents[0].count = 2; ents[0].stages = 1u << NVN_GPU_SH_FS;
    ents[1].set = 0; ents[1].cls = NVN_GPU_RES_SAMP; ents[1].access = NVN_GPU_ACC_R;
    ents[1].binding = 1; ents[1].count = 1; ents[1].stages = 1u << NVN_GPU_SH_FS;
    nvn_gpu_layout fl;
    memset(&fl, 0, sizeof fl);
    fl.ent = ents; fl.nent = 2;
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "arr2.frag", NULL, &fl, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 arr2 着色器"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C63 pipe：%d %s", e.rc.code, e.why);
    const uint8_t red[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    const uint8_t grn[16] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    nvn_gpu_tex tr = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_tex tg = make_tex(2, 2, NVN_FORMAT_RGBA8, NVN_GPU_TEX_SAMPLED | NVN_GPU_TEX_CPU_UPLOAD);
    nvn_gpu_view vr = make_view(tr), vg = make_view(tg);
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    nvn_gpu_samp samp = NULL;
    CHECK(OK(nvn_gpu_samp_create(&sp, &samp, &e)), "C63 samp：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_tex_upload(c, tr, 0, 0, NULL, red, 8, 16);
    nvn_gpu_tex_upload(c, tg, 0, 0, NULL, grn, 8, 16);
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_binding bs[3];
    memset(bs, 0, sizeof bs);
    bs[0].ent = 0; bs[0].cls = NVN_GPU_RES_TEX; bs[0].elem = 0; bs[0].u.view = vr;
    bs[1].ent = 0; bs[1].cls = NVN_GPU_RES_TEX; bs[1].elem = 1; bs[1].u.view = vg;
    bs[2].ent = 1; bs[2].cls = NVN_GPU_RES_SAMP; bs[2].u.samp = samp;
    nvn_gpu_bind(c, NVN_GPU_SH_FS, bs, 3);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 110, on_done, NULL, &e);
    CHECK(OK(r), "C63 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(110, 5000000000ull, &e);
    CHECK(OK(r), "C63 wait：%d %s", r.code, e.why);
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 255, 0, "C63");
    nvn_gpu_view_release(vr); nvn_gpu_view_release(vg);
    nvn_gpu_tex_release(tr); nvn_gpu_tex_release(tg);
    nvn_gpu_samp_release(samp);
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C64：VA 类别显式报红——契约未定非 BDA 消费方式，管线创建期必须 E_UNSUPPORTED，不许静默成功。
static void c64_va_red(const char *dir) {
    nvn_gpu_err e;
    // 计算管线
    {
        nvn_gpu_layout_ent ce;
        memset(&ce, 0, sizeof ce);
        ce.set = 0; ce.cls = NVN_GPU_RES_VA; ce.access = NVN_GPU_ACC_R; ce.binding = 0; ce.count = 1;
        ce.stages = 1u << NVN_GPU_SH_CS;
        nvn_gpu_layout cl;
        memset(&cl, 0, sizeof cl);
        cl.ent = &ce; cl.nent = 1;
        nvn_gpu_shader sh = load_one_shader(dir, "comp.comp", NVN_GPU_SH_CS, &cl);
        if (sh) {
            nvn_gpu_stage_desc cs;
            memset(&cs, 0, sizeof cs);
            cs.sh = sh;
            nvn_gpu_pipe cp = NULL;
            nvn_gpu_rc r = nvn_gpu_cpipe_get(&cs, NVN_GPU_PIPE_SYNC, &cp, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 计算 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(sh);
        }
    }
    // 图形管线
    {
        nvn_gpu_layout_ent fe;
        memset(&fe, 0, sizeof fe);
        fe.set = 0; fe.cls = NVN_GPU_RES_VA; fe.access = NVN_GPU_ACC_R; fe.binding = 0; fe.count = 1;
        fe.stages = 1u << NVN_GPU_SH_FS;
        nvn_gpu_layout fl;
        memset(&fl, 0, sizeof fl);
        fl.ent = &fe; fl.nent = 1;
        nvn_gpu_shader vs = NULL, fs = NULL;
        void *vm = NULL, *fm = NULL;
        if (load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, &fl, &vs, &fs, &vm, &fm)) {
            nvn_gpu_pipe_desc pd;
            fill_gpipe_desc(&pd, vs, fs, 0);
            nvn_gpu_pipe pipe = NULL;
            nvn_gpu_rc r = nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e);
            CHECK(r.code == NVN_GPU_E_UNSUPPORTED, "C64 图形 VA 应 E_UNSUPPORTED，实为 %d", r.code);
            nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
            free(vm); free(fm);
        }
    }
}

// ---- D9 静默错复现（只复现不修）：断言的是当前实际行为；修好对应项后这些用例要翻期望 ----

// C65 复现：ncolor=0 但带有效深度附件的 pass 被 pass_begin 判 !ncolor → E_ARG。
// 正确语义下应绿（depth-only pass 合法）；修好后把断言改成 OK + 深度读回核对。
static void c65_depth_only_pass(void) {
    nvn_gpu_tex t = make_tex(64, 64, NVN_FORMAT_DEPTH32F, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view v = make_view(t);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.depth.view = v; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 0.5f;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 111, on_done, NULL, &e);
    CHECK(r.code == NVN_GPU_E_ARG, "C65 深度单通道 pass 当前应 E_ARG（缺陷复现），实为 %d", r.code);
    nvn_gpu_view_release(v); nvn_gpu_tex_release(t);
}

// C66 已知缺陷：当前每条 cmd 的池固定 64 set 且不扩容。使用测试链接包装器模拟
// 严格驱动在第 65 个 set 返回 OUT_OF_POOL_MEMORY；不是声称 llvmpipe 自身拒绝。
static void c66_pool_sets(const char *dir) {
    nvn_gpu_layout_ent ce;
    memset(&ce, 0, sizeof ce);
    ce.set = 0; ce.cls = NVN_GPU_RES_SSBO; ce.access = NVN_GPU_ACC_W; ce.binding = 0; ce.count = 1;
    ce.stages = 1u << NVN_GPU_SH_CS;
    nvn_gpu_layout cl;
    memset(&cl, 0, sizeof cl);
    cl.ent = &ce; cl.nent = 1;
    nvn_gpu_shader sh = NULL;
    nvn_gpu_pipe cp = mk_cpipe(dir, "comp.comp", &cl, &sh);
    CHECK(cp != NULL, "C66 cpipe 为空");
    if (!cp) return;
    nvn_gpu_buf buf = make_buf(64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_set_pipe(c, cp);
    nvn_gpu_binding b;
    memset(&b, 0, sizeof b);
    b.ent = 0; b.cls = NVN_GPU_RES_SSBO; b.u.b.buf = buf; b.u.b.off = 0; b.u.b.len = 64 * 4;
    nvn_gpu_bind(c, NVN_GPU_SH_CS, &b, 1);
    g_c66_sets_requested = 0;
    g_c66_pool = VK_NULL_HANDLE;
    g_c66_inject_pool_limit = 1;
    for (int i = 0; i < 70; i++) nvn_gpu_dispatch(c, cp, 1, 1, 1, NULL, 0);
    g_c66_inject_pool_limit = 0;
    nvn_gpu_err e;
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 112, on_done, NULL, &e);
    CHECK(g_c66_sets_requested == 65, "C66 模拟严格驱动应第 65 个 set 拒绝，实为 %u", g_c66_sets_requested);
    CHECK(r.code == NVN_GPU_E_OOM, "C66 已知缺陷：模拟池满后当前应 E_OOM，实为 %d", r.code);
    printf("C66 已知缺陷（模拟严格池上限）：分配至第 %u 项，commit=%d，期望 E_OOM=%d\n",
           g_c66_sets_requested, r.code, NVN_GPU_E_OOM);
    nvn_gpu_buf_release(buf, NULL, NULL);
    nvn_gpu_shader_release(sh);
}

// C67 复现：origin_ll=1 的视口 {0,0,64,32} 没翻 Y。NVN 从左下量 → 正确应画下半屏；
// 当前按左上原点下达 → 画上半屏。修好后期望翻成下半屏绿。
static void c67_viewport_yflip(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, 0);
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C67 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_view ov = make_view(out);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[2] = 1; p.color[0].clear.f[3] = 1;   // 蓝底衬绿
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.nvp = 1;
    dyn.vp[0][0] = 0; dyn.vp[0][1] = 0; dyn.vp[0][2] = 64; dyn.vp[0][3] = 32; dyn.vp[0][4] = 0; dyn.vp[0][5] = 1;
    dyn.origin_ll = 1;
    dyn.has_sc = 1; dyn.sc[2] = 64; dyn.sc[3] = 64;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 113, on_done, NULL, &e);
    CHECK(OK(r), "C67 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(113, 5000000000ull, &e);
    CHECK(OK(r), "C67 wait：%d %s", r.code, e.why);
    // 复现当前（错）：上半屏绿、下半屏蓝。修好后应反过来。
    expect_rect(buf_cpu(rb), 0, 0, 64, 32, 0, 255, 0, "C67 上半（当前错，应为蓝）");
    expect_rect(buf_cpu(rb), 0, 32, 64, 32, 0, 0, 255, "C67 下半（当前错，应为绿）");
    nvn_gpu_view_release(ov); nvn_gpu_tex_release(out);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

// C68 复现：模板 op / compare / 掩码没进管线。front/back 全 0 → compareOp=NEVER → 全丢。
// 配置 sf=ALWAYS + zpass=REPLACE + ref=1 + 掩码 FF：正确应全绿；当前应零像素绿（全为清屏色）。
static void c68_stencil_ops(const char *dir) {
    nvn_gpu_shader vs = NULL, fs = NULL;
    void *vm = NULL, *fm = NULL;
    if (!load_shader_pair(dir, "tri3.vert", "solid_green.frag", NULL, NULL, &vs, &fs, &vm, &fm)) { CHECK(0, "读不到 solid_green"); return; }
    nvn_gpu_pipe_desc pd;
    fill_gpipe_desc(&pd, vs, fs, NVN_FORMAT_DEPTH24_STENCIL8);
    pd.ds.st = 1;
    pd.ds.sf_f = 8; pd.ds.sf_b = 8;            // NVN StencilFunc：8 = ALWAYS
    pd.ds.fail_f = 2; pd.ds.zfail_f = 2; pd.ds.zpass_f = 3;   // NVN op 值 = MTLStencilOperation+1：2=ZERO、3=REPLACE
    pd.ds.fail_b = 2; pd.ds.zfail_b = 2; pd.ds.zpass_b = 3;
    nvn_gpu_err e;
    nvn_gpu_pipe pipe = NULL;
    CHECK(OK(nvn_gpu_pipe_get(&pd, NVN_GPU_PIPE_SYNC, &pipe, &e)), "C68 pipe：%d %s", e.rc.code, e.why);
    nvn_gpu_tex out = make_tex(64, 64, NVN_FORMAT_RGBA8, NVN_GPU_TEX_RENDER | NVN_GPU_TEX_SAMPLED);
    nvn_gpu_tex ds = make_tex(64, 64, NVN_FORMAT_DEPTH24_STENCIL8, NVN_GPU_TEX_RENDER);
    nvn_gpu_view ov = make_view(out), dv = make_view(ds);
    nvn_gpu_buf rb = make_buf(64 * 64 * 4, NVN_GPU_MEM_SHARED, NULL);
    nvn_gpu_buf vb = full_vb();
    nvn_gpu_cmd c = begin_cmd();
    nvn_gpu_pass_desc p;
    memset(&p, 0, sizeof p);
    p.ncolor = 1; p.color[0].view = ov; p.color[0].load = NVN_GPU_LOAD_CLEAR; p.color[0].store = NVN_GPU_STORE_STORE;
    p.color[0].clear.f[3] = 1;
    p.depth.view = dv; p.depth.load = NVN_GPU_LOAD_CLEAR; p.depth.store = NVN_GPU_STORE_STORE;
    p.clear_depth = 1.0f; p.clear_stencil = 0;
    p.w = 64; p.h = 64; p.layers = 1;
    nvn_gpu_pass_begin(c, &p);
    nvn_gpu_set_pipe(c, pipe);
    nvn_gpu_dyn dyn;
    full_dyn(&dyn);
    dyn.st_vmask[0] = dyn.st_vmask[1] = 0xFF;
    dyn.st_wmask[0] = dyn.st_wmask[1] = 0xFF;
    dyn.st_ref[0] = dyn.st_ref[1] = 1;
    nvn_gpu_set_dyn(c, &dyn);
    nvn_gpu_set_vbuf(c, 0, vb, 0);
    nvn_gpu_draw_args da;
    memset(&da, 0, sizeof da);
    da.prim = 4; da.count = 3; da.instances = 1;
    nvn_gpu_draw(c, &da);
    nvn_gpu_pass_end(c);
    nvn_gpu_copy_tex_buf(c, out, 0, 0, NULL, rb, 0, 64 * 4, 64 * 64 * 4);
    nvn_gpu_rc r = nvn_gpu_cmd_commit(c, 114, on_done, NULL, &e);
    CHECK(OK(r), "C68 commit：%d %s", r.code, e.why);
    r = nvn_gpu_fence_wait(114, 5000000000ull, &e);
    CHECK(OK(r), "C68 wait：%d %s", r.code, e.why);
    // 复现当前（错）：管线 front/back 模板态全 0 → compareOp=NEVER → 一个像素都没画，
    // 全为清屏色（a=255,r=g=b=0）。修好（sf=ALWAYS + zpass=REPLACE + mask/ref 生效）应全绿。
    // 注：模板字节契约层读不回——copy_tex_buf 只能整 t->aspect 下 buffer copy，
    // 多 aspect 贴图会撞 VUID-VkBufferImageCopy-aspectMask-09103（只许单 aspect）。
    expect_rect(buf_cpu(rb), 0, 0, 64, 64, 0, 0, 0, "C68 颜色（已知缺陷：当前黑，修复后应绿）");
    printf("C68 已知缺陷：颜色为黑（修复后期望绿）；仅核颜色，未读 stencil\n");
    nvn_gpu_view_release(ov); nvn_gpu_view_release(dv);
    nvn_gpu_tex_release(out); nvn_gpu_tex_release(ds);
    nvn_gpu_buf_release(vb, NULL, NULL);
    nvn_gpu_buf_release(rb, NULL, NULL);
    nvn_gpu_shader_release(vs); nvn_gpu_shader_release(fs);
    free(vm); free(fm);
}

int main(int argc, char **argv) {
    t29_format_expand();
    char why[256] = {0};
    if (s2a_vk_device_create(&g_dev, why, sizeof why) != 0) {
        printf("SKIP? 建 Vulkan 设备失败：%s\n", why);
        return 2;   // 不静默跳过：run.sh 把 2 当红
    }
    printf("device: %s (%s)\n", g_dev.properties.deviceName, g_dev.driver_name);
    // 机型模拟档（android/tests/vk/sim_tiers.sh）按这行核对模拟层确实藏掉了对应能力。
    printf("sim caps: vertexPipelineStoresAndAtomics=%u dualSrcBlend=%u calibrated_timestamps=%d（0 = 无校准扩展，走完成点兜底）\n",
           (unsigned)g_dev.core_features.vertexPipelineStoresAndAtomics, (unsigned)g_dev.core_features.dualSrcBlend,
           g_dev.calibrated_timestamps);
    if (argc < 2) {
        printf("用法：%s <shader-dir>（run.sh 会编译 tri.vert/frag 到该目录）\n", argv[0]);
        return 2;
    }
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.host_device = &g_dev;
    g_desc.app_label = "nvn-backend-vk-test";
    if (!open_backend()) return 1;

    nvn_gpu_caps caps;
    nvn_gpu_caps_get(&caps);
    CHECK(caps.api == NVN_GPU_API_VULKAN, "caps.api 应为 VULKAN");
    CHECK(caps.max_sets > 0 && caps.max_buf_bytes > 0, "caps 限值没有填");
    CHECK(caps.caps_id[0] | caps.caps_id[1] | caps.caps_id[15], "caps_id 全 0");
    CHECK(nvn_gpu_format_caps(0x1) != 0, "format_caps 对 R8 无能力位");   // NVN_FORMAT_R8

    t01_fill32();
    t02_write_copy();
    t03_keepalive_unsubmitted();
    t03b_buf_use();
    t04_abort();
    t05_failure_terminal();

    nvn_gpu_err e;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第一次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t06_publish_order();
    t07_wait_args();
    t08_sticky();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e)), "第二次 close 失败：%d %s", e.rc.code, e.why);
    if (!open_backend()) return 1;
    t09_release_only();
    t11_clear_color();
    t12_upload_copy_tex();
    t13_clear_depth();
    t14_textured_triangle(argv[1]);
    t10_close_inflight();
    if (!open_backend()) return 1;
    t16_fence_failure();
    nvn_gpu_err e2;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e2)), "t16 close：%d %s", e2.rc.code, e2.why);
    if (!open_backend()) return 1;
    t17_close_timeout();
    if (!open_backend()) return 1;
    t18_combined_sampler(argv[1]);
    nvn_gpu_err e3;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t18 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t19_signal_contract();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e3)), "t19 close：%d %s", e3.rc.code, e3.why);
    if (!open_backend()) return 1;
    t20_combined_missing_samp(argv[1]);
    nvn_gpu_err e4;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e4)), "t20 close：%d %s", e4.rc.code, e4.why);
    if (!open_backend()) return 1;
    t21_clear_int();
    nvn_gpu_err e5;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e5)), "t21 close：%d %s", e5.rc.code, e5.why);
    if (!open_backend()) return 1;
    t22_clear_view_fmt();
    nvn_gpu_err e6;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e6)), "t22 close：%d %s", e6.rc.code, e6.why);
    if (!open_backend()) return 1;
    t30_rgb8_sampled(argv[1]);
    t28_rgb8_upload();
    nvn_gpu_err e7;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e7)), "t30/t28 close：%d %s", e7.rc.code, e7.why);
    if (!open_backend()) return 1;
    t31_vertex_formats(argv[1]);
    nvn_gpu_err e8;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e8)), "t31 close：%d %s", e8.rc.code, e8.why);
    if (!open_backend()) return 1;
    t32_expand_bounds();
    nvn_gpu_err e9;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e9)), "t32 close：%d %s", e9.rc.code, e9.why);
    if (!open_backend()) return 1;
    t33_two_passes(argv[1]);
    t34_two_pass_load(argv[1]);
    t35_three_passes(argv[1]);
    nvn_gpu_err e10;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e10)), "C33–C35 close：%d %s", e10.rc.code, e10.why);
    if (!open_backend()) return 1;
    c36_40(argv[1]);
    nvn_gpu_err e11;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e11)), "C36–C40 close：%d %s", e11.rc.code, e11.why);
    if (!open_backend()) return 1;
    c41_bounds_neg(argv[1]);
    nvn_gpu_err e12;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e12)), "C41 close：%d %s", e12.rc.code, e12.why);
    if (!open_backend()) return 1;
    c42_u8_indirect_neg(argv[1]);
    nvn_gpu_err e13;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e13)), "C42 close：%d %s", e13.rc.code, e13.why);
    if (!open_backend()) return 1;
    c43_blend(argv[1]);
    nvn_gpu_err e14;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e14)), "C43 close：%d %s", e14.rc.code, e14.why);
    if (!open_backend()) return 1;
    c44_prims(argv[1]);
    c46_blend_const(argv[1]);
    c47_logic(argv[1]);
    c48_fan(argv[1]);
    c49_restart(argv[1]);
    c50_list_restart(argv[1]);
    c51_array(argv[1]);
    c52_cube(argv[1]);
    c53_cube_array(argv[1]);
    c54_3d(argv[1]);
    c55_compute(argv[1]);
    nvn_gpu_err e15;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e15)), "C44 close：%d %s", e15.rc.code, e15.why);
    if (!open_backend()) return 1;
    c45_multi_blend(argv[1]);
    nvn_gpu_err e16;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e16)), "C45 close：%d %s", e16.rc.code, e16.why);
    if (!open_backend()) return 1;
    c48_mismatch(argv[1]);
    nvn_gpu_err e17;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e17)), "C48 mismatch close：%d %s", e17.rc.code, e17.why);
    if (!open_backend()) return 1;
    // 成功路径同会话；C58a–c / C65 / C66 预期失败提交，每个独占会话（失败段进终态，关会话重开恢复）。
    c56_push_fs(argv[1]);
    c57_push_cs(argv[1]);
    c58d_push_pipe_resize(argv[1]);
    c59a_buffer_view();
    c59_tbuf(argv[1]);
    c60_image_tbuf(argv[1]);
    c61_storage_image(argv[1]);
    c62_tsamp_cs(argv[1]);
    c63_desc_array_elem(argv[1]);
    c64_va_red(argv[1]);
    c67_viewport_yflip(argv[1]);
    c68_stencil_ops(argv[1]);
    nvn_gpu_err e18;
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C56–C68 成功路径 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58a_push_oob_cs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58a close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58b_push_bad_stage(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c58c_push_oob_fs(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C58c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c65_depth_only_pass();
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C65 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c66_pool_sets(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C66 close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60b_unknown_image_tbuf(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c60c_unknown_image_tbuf_read(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C60c close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c59b_legacy_binding(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C59b close：%d %s", e18.rc.code, e18.why);
    if (!open_backend()) return 1;
    c69_dual_src_blend(argv[1]);
    CHECK(OK(nvn_gpu_close(5000000000ull, &e18)), "C69 close：%d %s", e18.rc.code, e18.why);

    printf("nvn_gpu Vulkan 后端 C01–C08/C23/C25/C26/C27–C55/C56–C69 + 清屏/复制/深度/纹理三角形用例：%s（%d 处不符，done %d 次，retired %d 次）\n",
           g_fail ? "红" : "绿", g_fail, atomic_load(&g_ndone), atomic_load(&g_retired));
    s2a_vk_device_destroy(&g_dev, 1);   // 最后一个会话已在 t20 close 里证明空闲
    return g_fail ? 1 : 0;
}
