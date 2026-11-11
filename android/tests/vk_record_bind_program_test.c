// 生产入口反例（#48）：图形程序身份按阶段归属，只绑 CS / 空程序解绑别的阶段都不能换掉 draw 的程序。
//
// 为什么要有：OP_BIND_PROGRAM 曾每次都用本次调用的 prog 覆盖 bound_program。游戏先绑 VS/FS 程序，
// 再单独绑一个计算程序（stages = CS）或用空程序解绑几何/细分阶段后，下一笔 draw 交给后端的
// program 变成 CS 程序或空指针；真机上直连 draw builder 报「draw: no device/program」后 abort
// （FF RESONANCE 起局即中止）。Metal 侧 nvn_mtl.m state_op：stages & 0x1f 才换 S.prog，CS 单独归 S.cprog。
//
// 本测试走生产入口 s2a_record_exec_one，经生产接缝 s2a_record_set_draw_builder 读 draw 快照里的 program：
//   1) 绑图形程序 A → draw：program == A（基线）；
//   2) A 之后只绑 CS 程序 B → draw：program 仍是 A，计算侧另记 B（dispatch 不受影响由既有用例覆盖）；
//   3) A 之后空程序解绑几何阶段（stages = GS）→ draw：program 仍是 A；
//   4) 空程序解绑 VS/FS（全部图形阶段）→ draw 准确拒绝「without a bound program」，builder 不被调用；
//   5) 同一次调用同时含图形与 CS 阶段 → 图形程序换成本次程序。
//
// 测试替身边界：这些 opcode 只改 CPU 记录状态，不碰 Vulkan 对象（链接所需的 vk* 由宿主 Vulkan loader
// 解析，不建 device、不开 ICD）。Android 系统头由 tests/hostvk 提供。
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vk_record.h"
#include "nvn_obj.h"

// ---- 宿主替身：Android 日志 / 系统属性 / 显存类型选择（本路径不调用，只有链接需要）----
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio; (void)tag;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
    return 0;
}
int __system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }
uint32_t s2a_vk_memory_type(const struct s2a_vk_device *d, uint32_t bits,
                            VkMemoryPropertyFlags req, VkMemoryPropertyFlags *actual) {
    (void)d; (void)bits; (void)req; if (actual) *actual = 0; return 0;
}

// 程序快照（生产 compact 记录经 nvn_prog_snap_get 还原）：序号 < 100 = 图形程序，VS(0) + FS(1)；
// 序号 >= 100 = 计算程序，只有 CS(5)。
const prog_t *nvn_prog_snap_get(uint64_t serial, uint8_t keys[6][8], uint8_t *ok, uint8_t stage[6]) {
    static prog_t p;
    memset(&p, 0, sizeof p);
    p.serial = serial;
    memset(stage, 0xff, 6);
    memset(keys, (int)(serial & 0xff), 48);
    if (serial < 100) { stage[0] = 0; stage[1] = 1; *ok = 0x3; }
    else { stage[0] = 5; *ok = 0x1; }
    return &p;
}

static uint8_t g_guest[256];
static int rd(void *o, uint64_t a, void *b, size_t n) {
    (void)o; if (a + n > sizeof g_guest) return -1; memcpy(b, g_guest + a, n); return 0;
}
static int wr(void *o, uint64_t a, const void *b, size_t n) {
    (void)o; if (a + n > sizeof g_guest) return -1; memcpy(g_guest + a, b, n); return 0;
}

static struct s2a_record_draw_state g_cap;
static int g_builder_calls;
static int builder(struct s2a_vk_device *d, const struct s2a_record_draw_state *s,
                   struct s2a_record_result *r, void *opaque) {
    (void)d; (void)r; (void)opaque;
    g_builder_calls++;
    g_cap = *s;
    return 0;
}

static int g_checks, g_failed;
static void check(int cond, const char *what) {
    g_checks++;
    if (!cond) { g_failed++; fprintf(stderr, "FAIL: %s\n", what); }
}

static struct s2a_vk_device g_dev;
static struct s2a_record_memory g_mem = { NULL, rd, wr };

#define NVN_STAGE_VS NVN_SHADER_STAGE_VERTEX_BIT
#define NVN_STAGE_FS NVN_SHADER_STAGE_FRAGMENT_BIT
#define NVN_STAGE_GS NVN_SHADER_STAGE_GEOMETRY_BIT
#define NVN_STAGE_CS NVN_SHADER_STAGE_COMPUTE_BIT

static int bind(uintptr_t prog, uint64_t serial, int32_t stages) {
    uint8_t pkt[64];
    size_t size = sizeof(op_hdr) + sizeof(p_bind_program_id);
    memset(pkt, 0, sizeof pkt);
    op_hdr hd = { OP_BIND_PROGRAM, (uint32_t)size };
    memcpy(pkt, &hd, sizeof hd);
    p_bind_program_id id = { (const prog_t *)prog, serial, stages, 0 };
    memcpy(pkt + sizeof hd, &id, sizeof id);
    struct s2a_record_result r;
    memset(&r, 0, sizeof r);
    return s2a_record_exec_one(&g_dev, pkt, size, &g_mem, &r);
}

static int draw(char *why, size_t n) {
    uint8_t pkt[64];
    size_t size = sizeof(op_hdr) + offsetof(p_draw, ind);
    memset(pkt, 0, sizeof pkt);
    op_hdr hd = { OP_DRAW, (uint32_t)size };
    memcpy(pkt, &hd, sizeof hd);
    p_draw d;
    memset(&d, 0, sizeof d);
    d.prim = 4; d.count = 3; d.instances = 1;
    memcpy(pkt + sizeof hd, &d, offsetof(p_draw, ind));
    struct s2a_record_result r;
    memset(&r, 0, sizeof r);
    int rc = s2a_record_exec_one(&g_dev, pkt, size, &g_mem, &r);
    if (why && n) snprintf(why, n, "%s", r.why);
    return rc;
}

static void reset(void) {
    s2a_record_reset_state();
    memset(&g_cap, 0, sizeof g_cap);
    g_builder_calls = 0;
}

int main(void) {
    const uintptr_t A = 0x1000, B = 0x2000, C = 0x3000;
    char why[256];
    memset(&g_dev, 0, sizeof g_dev);
    g_dev.device = (VkDevice)(uintptr_t)1;   // 生产回放入口只要求非空句柄；这些 opcode 不调用 Vulkan
    s2a_record_set_draw_builder(builder, NULL);

    // 1) 基线：图形程序 A。
    reset();
    check(bind(A, 7, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "bind graphics program A");
    check(draw(why, sizeof why) == 0 && g_cap.program == (const void *)A, "draw sees program A");

    // 2) 只绑 CS 程序 B：图形程序仍是 A。
    reset();
    check(bind(A, 7, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "bind A before compute");
    check(bind(B, 107, NVN_STAGE_CS) == 0, "bind compute-only program B");
    check(draw(why, sizeof why) == 0, "draw after compute-only bind replays");
    check(g_cap.program == (const void *)A, "compute-only bind keeps graphics program A");
    check(g_cap.keys[0][0] == 7 && g_cap.keys[1][0] == 7, "graphics stage keys still from A");

    // 3) 空程序解绑 GS：VS/FS 还在，图形程序仍是 A。
    reset();
    check(bind(A, 7, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "bind A before unbinding GS");
    check(bind(0, 0, NVN_STAGE_GS) == 0, "null program unbinds GS");
    check(draw(why, sizeof why) == 0 && g_cap.program == (const void *)A, "null GS unbind keeps program A");

    // 4) 空程序解绑全部图形阶段：draw 准确拒绝，builder 不被调用。
    reset();
    check(bind(A, 7, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "bind A before full unbind");
    check(bind(0, 0, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "null program unbinds VS/FS");
    g_builder_calls = 0;
    check(draw(why, sizeof why) == S2A_RECORD_INVALID && g_builder_calls == 0,
          "draw without any graphics stage is rejected before the builder");

    // 5) 图形 + CS 同一次调用：图形程序换成本次程序 C。
    reset();
    check(bind(A, 7, NVN_STAGE_VS | NVN_STAGE_FS) == 0, "bind A before mixed bind");
    check(bind(C, 9, NVN_STAGE_VS | NVN_STAGE_FS | NVN_STAGE_CS) == 0, "mixed graphics+compute bind of C");
    check(draw(why, sizeof why) == 0 && g_cap.program == (const void *)C, "mixed bind replaces graphics program with C");

    s2a_record_set_draw_builder(NULL, NULL);
    printf("bind-program: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
