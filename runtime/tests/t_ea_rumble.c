// GameSir G8+ MFi 有线马达（ios/ea_rumble.h，#462）：配件判定与 9 字节组包。期望按协议资料手推，不跑被测实现得出
#include <stdio.h>
#include <string.h>
#include "ea_rumble.h"
static int fails, checks;
static void check(int ok, const char *what) { checks++; if (ok) printf("过 %s\n", what); else { printf("红 %s\n", what); fails++; } }
int main(void) {
    check(s2a_gamesir_is_target("GameSir", "G8+ MFi", 1), "① 厂商 GameSir、型号 G8+ MFi、声明协议 → 是");
    check(s2a_gamesir_is_target("gamesir", "g8+ mfi", 1), "② 大小写不同也认");
    check(!s2a_gamesir_is_target("GameSir", "G8+ MFi", 0), "③ 没声明 com.xiaoji.M2boot → 不是（开不了会话）");
    check(!s2a_gamesir_is_target("GameSir", "G8 Galileo", 1), "④ 同厂别的型号 → 不是");
    check(!s2a_gamesir_is_target(NULL, "G8+ MFi", 1) && !s2a_gamesir_is_target("GameSir", NULL, 1), "⑤ 缺厂商 / 型号 → 不是");
    uint8_t p[9];
    s2a_gamesir_packet(p, 1.0f, 0.5f);
    const uint8_t a[9] = {0x04, 255, 0x01, 128, 0x01, 0, 0, 0, 0};   // 0.5 × 255 = 127.5 → 四舍五入 128
    check(!memcmp(p, a, 9), "⑥ 左 1.0 右 0.5 → 04 FF 01 80 01 00 00 00 00");
    s2a_gamesir_packet(p, 0, 0);
    const uint8_t z[9] = {0x04, 0, 0x01, 0, 0x01, 0, 0, 0, 0};
    check(!memcmp(p, z, 9), "⑦ 停：两侧 0");
    check(s2a_gamesir_motor(-1) == 0 && s2a_gamesir_motor(2) == 255 && s2a_gamesir_motor(0.0f / 0.0f) == 0, "⑧ 越界与非数截到 0 / 255");
    check(s2a_gamesir_motor(0.002f) == 1 && s2a_gamesir_motor(0.001f) == 0, "⑨ 0.002 × 255 = 0.51 → 1；0.001 → 0.26 → 0");
    if (fails) { fprintf(stderr, "t_ea_rumble：%d/%d 项不符\n", fails, checks); return 1; }
    printf("✓ t_ea_rumble：%d 项（配件判定、组包、截断）\n", checks);
    return 0;
}
