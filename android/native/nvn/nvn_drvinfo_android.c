// 游戏 sdk 模块里的版本信息（SDK 主版本、游戏自带 NVN 驱动报的 API 版本等）在安卓上的强定义：
// 与苹果宿主（graphics/nvn/host_adapt.m）包含同一份 graphics/nvn/gfx/nvn/nvn_drvinfo.inc，不另写一套。
// 没有它时 hle_nvn.c 的弱缺省回「不知道」，NVN API 版本按新 SDK 报 55.x：SDK 10 及更早的游戏要 53.x，建设备前就中止。
#define _GNU_SOURCE
#include <string.h>
#include <strings.h>
#include "graphics/nvn/host/dm.h"
#include "graphics/nvn/gfx/nvn/nvn_drvscan.h"
#include "graphics/nvn/gfx/nvn/nvn2.h"
#include "graphics/nvn/gfx/nvn/nvn_drvinfo.inc"
