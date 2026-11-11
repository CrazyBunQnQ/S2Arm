// 从游戏自己的 sdk 模块读出来的版本信息：SDK 主版本、游戏自带 NVN 驱动报的设备信息（API 版本、GPU 代码版本范围、NvColorFormat 编码）、
//   Switch 2 的 NVN2 驱动常量项。hle_nvn.c / nvn2.c 里这些名字只有弱缺省（回 0 = 不知道），强定义在这里，两端同一份源码：
//   苹果由 graphics/nvn/host_adapt.m 包含，安卓由 android/native/nvn/nvn_drvinfo_android.c 包含。
//   做成 .inc 而不是单独的 .c：libnvn.a 不是整库载入，只含这几个名字的对象会因为弱缺省已满足引用而不被链接器取用，强定义悄悄失效。
//   包含方先带上：host/dm.h（LOG、g_mod / g_nmod）、gfx/nvn/nvn_drvscan.h、gfx/nvn/nvn2.h；用到 memmem / strncasecmp
// ---------- SDK 主版本（照上游 loader.c dm_sdk_major：sdk 模块只读数据里的 "NintendoSDK_nnSdk-<主版本>_…"）----------
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7)
            if (p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11)) {
                // Switch 2 的 SDK 先写变体名：「NintendoSdk_nnSdk-Generic_20_5_25」（Pokopia；同 the reference port loader.c）
                const char *v = p + 7; while (v < e && ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z'))) v++;
                if (v > p + 7 && v < e && *v == '_') v++; else v = p + 7;
                if (v < e && *v >= '0' && *v <= '9') { major = atoi(v); LOG("直连：SDK nnSdk-%.20s（主版本 %d）", p + 7, major); break; }
            }
    }
    if (!major) LOG("【红】直连：sdk 模块里没有 SDK 版本串，按新 SDK 处理");
    return major;
}

// ---------- 游戏自带 NVN 驱动报的设备信息（sdk 模块里驱动 nvnDeviceGetInteger 的立即数，nvn_drvscan.h）：API 版本、GPU 代码版本范围 ----------
// sdk 里驱动 nvnDeviceGetInteger 跳表的各条目立即数（nvn_drvscan.h；−1 = 该条目不是直接返回的常量），装载后第一次用到时扫一次
static int32_t g_drv_items[256]; static int g_drv_n = -1;
static int drv_items(void) {
    if (g_drv_n >= 0) return g_drv_n;
    g_drv_n = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk") && g_mod[i].base) {
        g_drv_n = nvn_drv_items_scan(g_mod[i].base, (size_t)g_mod[i].text_end, g_mod[i].size, g_drv_items, 256);
        if (!g_drv_n) LOG("直连：sdk 里没认出驱动的设备信息表，按 SDK 版本 / 宿主表报");
        else LOG("直连：游戏自带驱动报 NVN API %d.%d、GPU 代码版本 %d.%d–%d.%d（sdk 驱动常量，%d 项）", g_drv_items[0], g_drv_items[1],
                 g_drv_n > 0x31 ? g_drv_items[0x2F] : -1, g_drv_n > 0x31 ? g_drv_items[0x31] : -1, g_drv_n > 0x31 ? g_drv_items[0x2E] : -1, g_drv_n > 0x31 ? g_drv_items[0x30] : -1, g_drv_n);
    }
    return g_drv_n;
}
int dm_nvn_driver_api(int *major, int *minor) {
    if (drv_items() < 2) { *major = *minor = 0; return 0; }
    *major = g_drv_items[0]; *minor = g_drv_items[1]; return 1;
}
int dm_nvn_driver_item(int w, int *out) {
    if (w < 0 || w >= drv_items() || w >= 256 || g_drv_items[w] < 0) return 0;
    *out = g_drv_items[w]; return 1;
}
// 同一驱动 nvnTextureGetNvRmSurfaceNVX 给 RGBA8 的 NvColorFormat（nvn_drvscan.h；0 = 没认出）：直连层按它把宿主的 NvColorFormat 换成游戏这一代的编码（游戏的 VIC 库只认自己的）。
//   GOT 槽在装载后已重定位成绝对地址，扫描器按模块基址换回偏移
uint64_t dm_nvn_driver_nvrm_rgba8(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk") && g_mod[i].base)
        return nvn_drv_nvrm_rgba8_scan(g_mod[i].base, (size_t)g_mod[i].text_end, g_mod[i].size, (uint64_t)(uintptr_t)g_mod[i].base);
    return 0;
}

// ---------- Switch 2：游戏 sdk 里 NVN2 驱动 nvnDeviceGetInteger 的常量项（nvn2.c 用；移植自 the reference port loader.c dm_sdk_nvn2_item，MIT）----------
// 驱动的 switch 先把主版本装进 w0（mov w0, #231），再按字节表跳：adr L / adrp + add T / ldrb / add, lsl #2 / br，前面有 cmp wN, #最后一项。常量项是 mov w0, #v; b。
//   和 Switch 1 驱动的形状不同（nvn_drvscan.h：movz w0, #主版本 紧挨 br），所以单写一份。返回 1 并填 *out = 该项是常量；0 = 该项读别的（多数项查表或读 GPU 描述）或没认出 switch
int dm_nvn2_driver_item(unsigned item, int *out) {
    static nvn2_drv d; static int found = -1;
    if (found < 0) {
        found = 0;
        for (int i = 0; i < g_nmod && !found; i++) if (!strcmp(g_mod[i].name, "sdk") && g_mod[i].base) {
            found = nvn2_drv_scan(g_mod[i].base, (size_t)g_mod[i].ro_off, (size_t)g_mod[i].ro_off, (size_t)g_mod[i].key_page_off, &d);
            if (found) LOG("直连：Switch 2 sdk 里认出 NVN2 驱动的设备信息跳表（sdk+%#zx，%u 项）", d.at, d.last + 1);
        }
        if (nvn2_on() && !found) LOG("【红】直连：sdk 里没认出 NVN2 驱动 nvnDeviceGetInteger 的跳表，设备值按已知最新报");
    }
    return found && nvn2_drv_item(&d, item, out);
}
