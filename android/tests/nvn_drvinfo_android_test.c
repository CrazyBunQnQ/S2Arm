// 安卓上「游戏 sdk 模块里的版本信息」强定义（native/nvn/nvn_drvinfo_android.c，与苹果共用
// graphics/nvn/gfx/nvn/nvn_drvinfo.inc）。没有它时 hle_nvn.c 链到弱缺省 0，NVN API 版本一律
// 按新 SDK 报 55.x，SDK 10 及更早的游戏（要 53.x）建设备前就中止。
// 每个场景一个子进程：被测函数把结果记在静态变量里，一个进程只读一次。
// 只测版本串解析与「没有驱动表时如实回不知道」；驱动跳表的扫描在 graphics/nvn/tests/t_nvn_drvscan.c。
#define S2A_LOGCAP_IMPL
#include "../../runtime/src/native.h"
#include "../../graphics/nvn/gfx/nvn/nvn2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

S2A_TEST_MOD_TABLE(2); int g_nmod;
void s2a_log_out(int fd, const char *p, size_t n) { ssize_t w = write(fd, p, n); (void)w; }
// 本测试不涉及 Switch 2 的驱动表：三个 nvn2 入口给「不是 Switch 2、没认出」的替身
int nvn2_on(void) { return 0; }
int nvn2_drv_scan(const uint8_t *img, size_t text_end, size_t ro_lo, size_t ro_hi, nvn2_drv *d) {
    (void)img; (void)text_end; (void)ro_lo; (void)ro_hi; (void)d; return 0;
}
int nvn2_drv_item(const nvn2_drv *d, unsigned item, int *out) { (void)d; (void)item; (void)out; return 0; }

int dm_sdk_major(void);
int dm_nvn_driver_api(int *major, int *minor);
int dm_nvn_driver_item(int w, int *out);

enum { TEXT_END = 0x1000, RO = 0x1000, KEY = 0x3000, SIZE = 0x4000 };
static uint8_t sdk[SIZE], other[SIZE];

static void module(int i, const char *name, uint8_t *img) {
    module_t *m = &g_mod[i];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = img; m->size = SIZE; m->text_end = TEXT_END; m->ro_off = RO; m->key_page_off = KEY;
}

// 在子进程里摆好模块表、取一次 SDK 主版本，退出码 = 主版本
static int major_of(const char *sdk_ro, const char *main_ro) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        // 文本段填 nop：驱动跳表扫描认不出任何表
        for (size_t o = 0; o < TEXT_END; o += 4) { const uint32_t nop = 0xd503201f; memcpy(sdk + o, &nop, 4); }
        if (main_ro) memcpy(other + RO + 0x40, main_ro, strlen(main_ro));
        if (sdk_ro) memcpy(sdk + RO + 0x100, sdk_ro, strlen(sdk_ro));
        module(0, "main", other);
        module(1, "sdk", sdk);
        g_nmod = 2;
        int major = dm_sdk_major(), api[2] = {7, 7}, item = 7;
        if (dm_nvn_driver_api(&api[0], &api[1]) || api[0] || api[1]) _exit(101);   // 没有驱动表：回 0，并把两个出参清 0
        if (dm_nvn_driver_item(0, &item) || item != 7) _exit(102);                   // 同上：回 0，不动出参
        if (dm_sdk_major() != major) _exit(103);                                     // 第二次取同值
        _exit(major);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

int main(void) {
    // Switch 1：sdk 只读数据里的 "NintendoSDK_nnSdk-<主>_<次>_<修>"
    assert(major_of("SDK MW+Nintendo+NintendoSDK_nnSdk-5_3_0-Release", NULL) == 5);
    assert(major_of("SDK MW+Nintendo+NintendoSDK_nnSdk-10_4_0-Release", NULL) == 10);
    assert(major_of("SDK MW+Nintendo+NintendoSDK_nnSdk-13_3_5-Release", NULL) == 13);
    // Switch 2：先写变体名，再是主版本
    assert(major_of("SDK MW+Nintendo+NintendoSdk_nnSdk-Generic_20_5_25-Release", NULL) == 20);
    // 版本串只认 sdk 模块的：别的模块里有、sdk 里没有 → 0（不知道）
    assert(major_of(NULL, "SDK MW+Nintendo+NintendoSDK_nnSdk-5_3_0-Release") == 0);
    // 前缀不是 NintendoSDK 的同形串不算
    assert(major_of("SomebodyElse_nnSdk-5_3_0", NULL) == 0);
    puts("nvn_drvinfo_android_test: 6 scenarios ok");
    return 0;
}
