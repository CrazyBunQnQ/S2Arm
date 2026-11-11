// 纹理池里「没被写过的渲染目标读 0」只看这个对象自己的序号（nvn_mtl.m pool_unwritten_target）。NVN 里贴图对象只是描述，内容在存储里：
//   游戏用对象 A 当渲染目标画，再在同一段存储、同样的描述上另建对象 B 登记进纹理池给着色器采（只差用途标志，如 COMPRESSIBLE），B 读到的就是 A 画的内容。
// 期望（按 NVN 语义手写，不由被测实现推出）：
//   ① A 已画（共享表里有这段存储的宿主贴图）→ 按池取 B：拿到 A 那张宿主贴图（不是占位）
//   ② 对照：同样标 COMPRESSIBLE、存储上从没建过贴图的 C → 按池取：占位（不为读而建，t_nvn_pool_tex ④ 的语义不变）
#include "../gfx/nvn/nvn_mtl.m"

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "SETUP FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(2); } } while (0)
#define CHECK(c, ...) do { checks++; int ok_ = !!(c); failures += !ok_; printf("%s ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
static void make(tex_t *t, pool_t *pool, size_t off, uint32_t flags) {
    memset(t, 0, sizeof *t); t->magic = MAGIC('T','E','X',0); t->serial = nvn_new_serial();
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .flags = flags, .width = 64, .height = 64, .depth = 1, .levels = 1};
    t->pool = pool; t->off = off; t->mem = pool->mem + off; nvn_tex_layout(&t->d, &t->L); nvn_be_tex_init(t);
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0); alarm(60);
    mtl_init(); printf("BACKEND %s\n", g_m3 ? "Metal3" : "Metal4");
    uint8_t *mem = NULL; REQUIRE(!posix_memalign((void **)&mem, 65536, 1 << 20)); memset(mem, 0, 1 << 20);
    pool_t *pool = calloc(1, sizeof *pool); REQUIRE(pool);
    *pool = (pool_t){.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = mem, .size = 1 << 20};
    nvn_be_pool_add(mem, 1 << 20);
    nvn_be_sub_begin();
    tex_t A, B, C; make(&A, pool, 0, 0); make(&B, pool, 0, NVN_TEXTURE_FLAGS_COMPRESSIBLE); make(&C, pool, 262144, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    id<MTLTexture> a = tex_mtl(&A); REQUIRE(a); gpw_mark(&A, 0, 0, 1, 0, 0, 0, 0);   // A drawn into (a pass's attachment)
    nvn_be_reg_tex(5, &B, NULL); nvn_be_reg_tex(6, &C, NULL);
    pthread_mutex_lock(&g_rmu);
    int left = 0; uint64_t rid = 0; void *b = pool_tex_obj(5, UINT64_MAX, &left, &rid);
    int left2 = 0; uint64_t rid2 = 0; void *c = pool_tex_obj(6, UINT64_MAX, &left2, &rid2);
    pthread_mutex_unlock(&g_rmu);
    id<MTLTexture> bt = (__bridge id<MTLTexture>)b; id<MTLTexture> bb = bt.parentTexture ? bt.parentTexture : bt;
    CHECK(b && bb == a, "① A 已画、B 同存储同描述只多 COMPRESSIBLE：按池取 B 拿到 A 那张宿主贴图（取到 %p，A %p）", b, (__bridge void *)a);
    CHECK(!c && !tex_get(C.serial), "② 存储上从没建过贴图的 COMPRESSIBLE 目标 C：占位、没为读而建");
    printf("%s t_nvn_pool_rt_alias: %u checks %u failures; backend %s\n", failures ? "✗" : "✓", checks, failures, g_m3 ? "Metal3" : "Metal4");
    return failures != 0;
} }
