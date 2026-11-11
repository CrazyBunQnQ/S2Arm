// GameSir G8+ MFi 有线版的实体马达（#462）：系统 GameController / Core Haptics 把这只手柄的震动送到机身触觉
//   （系统日志 gamecontrollerd「Using internal actuator haptic destination」），手柄马达只认它公布的配件协议。
//   协议按玩家公开的资料自写，未在我方实机验证：配件协议名 com.xiaoji.M2boot，厂商 GameSir、型号 G8+ MFi；
//   每次写 9 字节「04 左马达 01 右马达 01 00 00 00 00」，马达强度 0–255。本头只放纯 C 的判定与组包（单测 runtime/tests/t_ea_rumble.c），
//   会话与写入在 ios/ea_rumble.m
#pragma once
#include <stdint.h>
#include <strings.h>

#define S2A_GAMESIR_PROTOCOL "com.xiaoji.M2boot"

// 配件是不是这只手柄：厂商、型号按字面（不分大小写）比，且声明了协议
static inline int s2a_gamesir_is_target(const char *manufacturer, const char *model, int has_protocol) {
    return has_protocol && manufacturer && model && !strcasecmp(manufacturer, "GameSir") && !strcasecmp(model, "G8+ MFi");
}
// 0–1 的振幅 → 马达字节（四舍五入，越界 / 非数按 0 与 1 截）
static inline uint8_t s2a_gamesir_motor(float amplitude) {
    if (!(amplitude > 0)) return 0;
    if (amplitude >= 1) return 255;
    return (uint8_t)(amplitude * 255.0f + 0.5f);
}
static inline void s2a_gamesir_packet(uint8_t out[9], float left, float right) {
    out[0] = 0x04; out[1] = s2a_gamesir_motor(left); out[2] = 0x01; out[3] = s2a_gamesir_motor(right); out[4] = 0x01;
    out[5] = out[6] = out[7] = out[8] = 0;
}

#ifdef __OBJC__
// 主线程刷新「当前接着的是不是这只手柄」（换手柄 / 配件插拔时调）；震动队列只读这个结果，不逐帧枚举配件
void s2a_ea_rumble_refresh(void);
int s2a_ea_rumble_active(void);
// 两侧振幅（已按档位缩放，0–1）；同一包不重发，写不进就留最新一包等可写
void s2a_ea_rumble_set(float left, float right);
void s2a_ea_rumble_stop(void);
#endif
