// 缓冲纹理按 2D 行读（wrapped 1D fetch → tbuf_as_2d）时池内偏移不满足线性贴图对齐：11-09 #309 火焰纹章 风花雪月，iPhone 17 Pro Max 光照逐帧亮暗交替。
//   玩家日志「texture buffer (texture ID 315, 512 elements of format 0x2e at pool offset 89890080) has no 2D-row form … the slot reads 0」：89890080 % 64 = 32，
//   iPhone 线性贴图要 64 字节对齐（Mac 16），原先回 nil、这一槽读 0。现在取同一段字节的对齐副本（帧暂存）建视图。
// 期望（按上面的语义手写，与被测实现无关）：对齐按 iPhone 抬到 64（DM_NVN_LINEAR_ALIGN=64）
//   ① 偏移 0x10020（差 32 不对齐）的 512 个 R32F 元素：返回非空视图；视图的缓冲偏移是 64 的倍数（不是池缓冲上的 0x10020）
//   ② 提交完成后视图背后的字节 = 源数据逐元素（第 i 个 = 1000 + i）
//   ③ 对照：偏移 0x20000（对齐）的同样纹理照旧原地建视图（视图的缓冲就是池缓冲、偏移 0x20000）
// 反证（修前）：同一单测编改前的 nvn_mtl.m → ① 返回 nil（「has no 2D-row form … the slot reads 0」）
#include "../gfx/nvn/nvn_mtl.m"

static unsigned checks, fails;
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL setup %s:%d %s\n", __FILE__, __LINE__, #x); exit(2); } } while (0)
#define VERIFY(c, ...) do { checks++; int ok_ = !!(c); if (!ok_) fails++; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
enum { MEM = 1 << 20, N = 512 };
static uint8_t *memory; static pool_t *pool;

static void buffer_texture(tex_t *t, int id, size_t off) {
    float *v = (float *)(memory + off); for (int i = 0; i < N; i++) v[i] = 1000.0f + (float)i;
    memset(t, 0, sizeof *t); t->magic = MAGIC('T', 'E', 'X', 0); t->serial = nvn_new_serial();
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_BUFFER, .format = NVN_FORMAT_R32F, .width = N, .height = 1, .depth = 1, .levels = 1};
    t->pool = pool; t->off = off; t->mem = pool->mem + off; nvn_tex_layout(&t->d, &t->L); nvn_be_tex_init(t);
    nvn_be_reg_tex(id, t, NULL);
}

int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setenv("DM_NVN_LINEAR_ALIGN", "64", 1);   // iPhone 的线性贴图对齐
    mtl_init();
    REQUIRE(!posix_memalign((void **)&memory, 65536, MEM)); memset(memory, 0, MEM);
    pool = calloc(1, sizeof *pool); REQUIRE(pool);
    *pool = (pool_t){.magic = MAGIC('P', 'O', 'O', 'L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = memory, .size = MEM};
    nvn_be_pool_add(memory, MEM);
    static tex_t a, b;
    buffer_texture(&a, 7, 0x10020); buffer_texture(&b, 8, 0x20000);
    REQUIRE(linear_align(MTLPixelFormatR32Float) == 64);
    nvn_be_sub_begin();
    id<MTLTexture> ta = tex_mtl(&a), tb = tex_mtl(&b); REQUIRE(ta && tb);
    REQUIRE(ta.textureType == MTLTextureTypeTextureBuffer && ta.bufferOffset == 0x10020);
    uint64_t ra = 0, rb = 0;
    id<MTLTexture> va = tbuf_as_2d(7, ta, 0, &ra), vb = tbuf_as_2d(8, tb, 0, &rb);
    VERIFY(va != nil && va.buffer != nil && va.bufferOffset % 64 == 0 && va.buffer != ta.buffer,
           "① 不对齐偏移 0x10020：视图 %s、缓冲偏移 %lu（期望 64 的倍数、不是池缓冲）", va ? "非空" : "空", (unsigned long)va.bufferOffset);
    VERIFY(vb != nil && vb.buffer == tb.buffer && vb.bufferOffset == 0x20000, "③ 对齐偏移 0x20000：原地视图（缓冲偏移 %lu，期望 131072、池缓冲本身）", (unsigned long)vb.bufferOffset);
    uint64_t f = nvn_be_sub_commit(); REQUIRE(nvn_be_wait(f, 10000000000ull)); REQUIRE(!mtl_last_commit_error());
    int bad = -1;
    if (va) { const float *got = (const float *)((const uint8_t *)va.buffer.contents + va.bufferOffset); for (int i = 0; i < N; i++) if (got[i] != 1000.0f + (float)i) { bad = i; break; } }
    VERIFY(va && bad < 0, "② 副本逐元素 = 源数据（%d 个；第一处不符 %d）", N, bad);
    printf("%s t_nvn_tbuf_align：%u 项，%u 项失败（不对齐池偏移的缓冲纹理按 2D 行读取对齐副本，#309）\n", fails ? "✗" : "✓", checks, fails);
    return fails != 0;
} }
