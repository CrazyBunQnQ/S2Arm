// #199 Metal 建不出的贴图视图按空槽处理，不让 Metal 断言结束进程（nvn_mtl.m tex_by_id）。
// 背景：一款足球游戏三个版本都在同一处闪退：NVN 视图把目标设成立方体，底下是只有 1 层的 2D 贴图，Metal 断言「newSliceRange.length(1) must be a multiple of (6)」。
// Ryujinx：目标维度对不上的视图不绑（Texture.GetTargetTexture 返回 null），槽读 0。
// 期望（按 Metal 的视图规则与 Ryujinx 语义手写，不由被测实现推出）：
//   ① 1 层 2D 贴图 + 立方体视图 → 取到 nil（空槽），进程不断言
//   ② 6 层 2D 数组 + 立方体视图 → 取到立方体视图（合法的照旧建）
//   ③ 6 层 2D 数组 + 立方体视图、层从 1 开始 → 越过末层 → nil
//   ④ 2D 贴图 + 3D 视图 → nil；⑤ 2D 贴图无视图 → 原样 2D
#include "../gfx/nvn/nvn_mtl.m"

static unsigned checks, failures;
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "SETUP FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(2); } } while (0)
#define CHECK(c, ...) do { checks++; int good_ = !!(c); failures += !good_; printf("%s ", good_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)
static void make(tex_t *t, pool_t *pool, size_t off, int target, int depth) {
    memset(t, 0, sizeof *t); t->magic = MAGIC('T','E','X',0); t->serial = nvn_new_serial();
    t->d = (nvn_texdesc){.target = target, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = depth, .levels = 1};
    t->pool = pool; t->off = off; t->mem = pool->mem + off; nvn_tex_layout(&t->d, &t->L); nvn_be_tex_init(t); REQUIRE(tex_mtl(t));
}
static id<MTLTexture> get(int tid) { pthread_mutex_lock(&g_rmu); id<MTLTexture> t = tex_by_id(tid); pthread_mutex_unlock(&g_rmu); return t; }
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IOLBF, 0); alarm(60);
    mtl_init(); printf("BACKEND %s\n", g_m3 ? "Metal3" : "Metal4");
    uint8_t *mem = NULL; REQUIRE(!posix_memalign((void **)&mem, 65536, 1 << 20)); memset(mem, 0, 1 << 20);
    pool_t *pool = calloc(1, sizeof *pool); REQUIRE(pool);
    *pool = (pool_t){.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = mem, .size = 1 << 20};
    nvn_be_pool_add(mem, 1 << 20);
    tex_t flat, arr; make(&flat, pool, 0, NVN_TEXTURE_TARGET_2D, 1); make(&arr, pool, 65536, NVN_TEXTURE_TARGET_2D_ARRAY, 6);
    view_t cube = {.set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP};
    view_t cube1 = {.set_target = 1, .target = NVN_TEXTURE_TARGET_CUBEMAP, .set_layers = 1, .min_layer = 1, .nlayers = 6};
    view_t vol = {.set_target = 1, .target = NVN_TEXTURE_TARGET_3D};
    nvn_be_reg_tex(1, &flat, &cube); nvn_be_reg_tex(2, &arr, &cube); nvn_be_reg_tex(3, &arr, &cube1); nvn_be_reg_tex(4, &flat, &vol); nvn_be_reg_tex(5, &flat, NULL);
    CHECK(get(1) == nil, "① 1 层 2D + 立方体视图：空槽，进程没断言");
    id<MTLTexture> c = get(2); CHECK(c && c.textureType == MTLTextureTypeCube, "② 6 层数组 + 立方体视图：照旧建（type %lu）", (unsigned long)c.textureType);
    CHECK(get(3) == nil, "③ 层 1 起 6 层越过末层：空槽");
    CHECK(get(4) == nil, "④ 2D + 3D 视图：空槽");
    id<MTLTexture> f = get(5); CHECK(f && f.textureType == MTLTextureType2D, "⑤ 无视图：原样 2D");
    printf("%s t_nvn_view_fit: %u checks %u failures; backend %s\n", failures ? "✗" : "✓", checks, failures, g_m3 ? "Metal3" : "Metal4");
    return failures != 0;
} }
