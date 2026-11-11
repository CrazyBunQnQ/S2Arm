// 图形模式（docs/设计_直连架构.md §12.1）：直连 = NVN / Vulkan / OpenGL → Metal（graphics/nvn）。只在起局时判定一次（装载器重定位之前）。
// 11-07 拆通用模式（玩家入口全删）：包里没有本宿主口径的直连库时 s2a_gfx_mode 回 S2A_GFX_GENERIC（= 不能直连），
//   真起局由 host_main.c 报「游戏文件缺失」收局；通用渲染器本体（graphics/metal、Ryujinx 显卡模拟）这次未删，安卓自有实现仍用这两个取值。
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { S2A_GFX_GENERIC = 0, S2A_GFX_DIRECT = 1 };
// 本次运行的图形模式。判定：环境变量 S2A_GFX=direct（开发 / 单测强制直连）→ 包里带本链直连库 → 直连，否则不能直连。
//   判定时 host.log 打一行「图形模式：…（依据 …）」；旧版本留下的 wire/<程序号>/{fail,fail_seen,mode} 起局时删掉、不作依据
int s2a_gfx_mode(void);
void s2a_gfx_mode_data_ready(void);   // 资源包闸门放行后、装载前调（iOS）；之前 s2a_gfx_mode 只给临时的「不能直连」、不落定
int s2a_gfx_mode_settled(void);       // 模式能落定了没有（提示类按它等，别拿临时值判）
// 本次判定的依据（同 host.log「图形模式：…（依据 …）」，字符串常量，不用释放）
const char *s2a_gfx_mode_why(void);
const char *s2a_gfx_mode_brief(void);   // 判定依据短写（开发浮层）
// 包里带着本宿主翻译链版本的直连库（<游戏数据>/shaders/direct/b-<链版本>/，或 Vulkan vk/、OpenGL gl/）且链进了直连图形层、设备跑得了直连
int s2a_direct_packaged(void);
// 直连模式已上屏的帧数（vi GetLatestFrameNumber 读）；通用模式恒 0
unsigned long long s2a_direct_frame_count(void);
// App 进 / 出后台（ios/app.m）：直连模式下暂停 / 恢复图形提交；通用模式空操作。
// gate 只关 / 开新入口，不等在途调用，供 UIKit 后台通知在返回前同步调用；suspend(1) 在工作队列上等旧调用并排空 GPU。
void s2a_direct_suspend_gate(int on);
void s2a_direct_suspend(int on);
// 重启的新 provider 已就绪、guest 尚未启动：只关直连提交 gate 并确认没有在途调用，不排空或提交 Metal 命令。
int s2a_direct_guest_start_hold(void);
#ifdef __cplusplus
}
#endif
