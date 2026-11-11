// OpenGL 前端公共部分：对象表、错误码、计数、函数表查找（eglGetProcAddress 与装载器共用）。接口见 docs/研究/GL前端接口.md
#include "gl_int.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

__thread gl_state *gl_cur;   // 按线程（egl.m 文件头）
gl_names gl_buffers, gl_vaos, gl_shaders, gl_programs;
_Atomic uint64_t gl_n_frames, gl_n_draws, gl_n_passes, gl_bad_feat, gl_bad_err;

// ---------- 对象表 ----------
void *gl_name_get(gl_names *t, GLuint name) { return name && name < t->n ? t->v[name] : NULL; }
static void grow(gl_names *t, uint32_t need) {
    if (need <= t->cap) return;
    uint64_t c = t->cap ? t->cap : 64; while (c < need) c *= 2;
    void **v = realloc(t->v, c * sizeof *v); if (!v) die("[gl] 对象表扩容失败（%llu）", (unsigned long long)c);
    memset(v + t->cap, 0, (c - t->cap) * sizeof *v); t->v = v; t->cap = (uint32_t)c;
}
GLuint gl_name_new(gl_names *t, void *obj) {
    if (!obj) { gl_set_error(0x0505); return 0; }   // GL_OUT_OF_MEMORY：不发布空槽，也不扩表
    if (!t->n) t->n = 1;   // 0 号不用（GL 的 0 = 没有对象）
    for (uint32_t i = 1; i < t->n; i++) if (!t->v[i]) { t->v[i] = obj; return i; }
    grow(t, t->n + 1); t->v[t->n] = obj; return t->n++;
}
// 表是按名字下标的稠密数组：Gen 出的名字从 1 连号，绑定自选名字（兼容配置允许）超过 GL_NAME_DENSE_MAX 就拒绝，不为一个大名字开整张表，
//   也不让 name + 1（UINT32_MAX 回绕成 0）和扩容倍增回绕（wwmogu #360 复审指出）
int gl_name_put(gl_names *t, GLuint name, void *obj) {
    if (!name) return 1;
    if (name >= GL_NAME_DENSE_MAX) { GLRED1("对象名字 %u 超过稠密表上限 %u：不建（报 GL_OUT_OF_MEMORY）", name, GL_NAME_DENSE_MAX); return 0; }
    grow(t, name + 1); if (t->n <= name) t->n = name + 1; t->v[name] = obj; return 1;
}
void *gl_name_del(gl_names *t, GLuint name) { void *o = gl_name_get(t, name); if (o) t->v[name] = NULL; return o; }
void gl_names_clear(gl_names *t, void (*destroy)(void *)) {
    for (uint32_t i = 1; i < t->n; i++) if (t->v[i]) destroy(t->v[i]);
    free(t->v); *t = (gl_names){0};
}

// ---------- 错误 ----------
#undef gl_set_error
void gl_set_error(GLenum e) { if (gl_cur && !gl_cur->error) gl_cur->error = e; }
void gl_set_error_at(GLenum e, const char *fn) {
    if (!gl_cur || gl_cur->error) return;
    gl_cur->error = e;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) GLOG("GL 错误 %#x 由 %s 置（游戏 glGetError 会读到）", e, fn ? fn : "?");
}
GL(glGetError, GLenum, void) { if (!gl_cur) return 0; GLenum e = gl_cur->error; gl_cur->error = 0; return e; }

// ---------- 对公共部分的导出 ----------
void dm_gl_stats(uint64_t *frames, uint64_t *draws, uint64_t *passes) { *frames = gl_n_frames; *draws = gl_n_draws; *passes = gl_n_passes; }
void dm_gl_bad_draws(uint64_t *feat, uint64_t *err) { *feat = gl_bad_feat; *err = gl_bad_err; }

// ---------- 函数表查找 ----------
// 引用各片的锚（gl_int.h「分片锚」）：静态链接时每片的登记段都进来（used：没人读也留着这些引用）
__attribute__((used)) static const char *const gl_slices[] = {&gl_slice_egl, &gl_slice_gl_buf, &gl_slice_gl_draw, &gl_slice_gl_fb, &gl_slice_gl_prog, &gl_slice_gl_tex};
// __DATA,swgl：GL / GLB 宏登记的（名字 → 实现）。没有的名字回 NULL（GL 规范允许取不到扩展函数），每个名字记一次日志，供普查差集
// 核心函数（名字结尾不是两个以上大写字母的厂商后缀 ARB / EXT / NV …）没实现时不回空：游戏按报的 GL 版本直接调，拿到空指针就跳到 0、
//   iOS 当场按代码签名杀掉（小丑牌 v0.2.6 玩家 iPhone 5 次起局都这样）。交一个预编好的空实现：第一次调用打红点名、回 0，
//   日志直接看出游戏用了哪个，再给真实现。扩展函数照旧回空（GL 允许取不到，程序常按指针判支持）。256 个用完退回空
static int gl_core_name(const char *n) {
    size_t l = strlen(n), u = 0; while (u < l && n[l - 1 - u] >= 'A' && n[l - 1 - u] <= 'Z') u++;
    return u < 2 && !strncmp(n, "gl", 2);
}
static const char *g_gl_miss_name[256];
static __attribute__((noinline)) uint64_t gl_miss_call(int i) {
    static _Atomic uint8_t said[256]; if (!atomic_exchange(&said[i], 1)) GLOG("【红】 游戏调了没实现的 GL 核心函数 %s（空实现，什么都不做、回 0）", g_gl_miss_name[i]);
    return 0;
}
static uint64_t gl_miss_0(void) { return gl_miss_call(0); }
static uint64_t gl_miss_1(void) { return gl_miss_call(1); }
static uint64_t gl_miss_2(void) { return gl_miss_call(2); }
static uint64_t gl_miss_3(void) { return gl_miss_call(3); }
static uint64_t gl_miss_4(void) { return gl_miss_call(4); }
static uint64_t gl_miss_5(void) { return gl_miss_call(5); }
static uint64_t gl_miss_6(void) { return gl_miss_call(6); }
static uint64_t gl_miss_7(void) { return gl_miss_call(7); }
static uint64_t gl_miss_8(void) { return gl_miss_call(8); }
static uint64_t gl_miss_9(void) { return gl_miss_call(9); }
static uint64_t gl_miss_10(void) { return gl_miss_call(10); }
static uint64_t gl_miss_11(void) { return gl_miss_call(11); }
static uint64_t gl_miss_12(void) { return gl_miss_call(12); }
static uint64_t gl_miss_13(void) { return gl_miss_call(13); }
static uint64_t gl_miss_14(void) { return gl_miss_call(14); }
static uint64_t gl_miss_15(void) { return gl_miss_call(15); }
static uint64_t gl_miss_16(void) { return gl_miss_call(16); }
static uint64_t gl_miss_17(void) { return gl_miss_call(17); }
static uint64_t gl_miss_18(void) { return gl_miss_call(18); }
static uint64_t gl_miss_19(void) { return gl_miss_call(19); }
static uint64_t gl_miss_20(void) { return gl_miss_call(20); }
static uint64_t gl_miss_21(void) { return gl_miss_call(21); }
static uint64_t gl_miss_22(void) { return gl_miss_call(22); }
static uint64_t gl_miss_23(void) { return gl_miss_call(23); }
static uint64_t gl_miss_24(void) { return gl_miss_call(24); }
static uint64_t gl_miss_25(void) { return gl_miss_call(25); }
static uint64_t gl_miss_26(void) { return gl_miss_call(26); }
static uint64_t gl_miss_27(void) { return gl_miss_call(27); }
static uint64_t gl_miss_28(void) { return gl_miss_call(28); }
static uint64_t gl_miss_29(void) { return gl_miss_call(29); }
static uint64_t gl_miss_30(void) { return gl_miss_call(30); }
static uint64_t gl_miss_31(void) { return gl_miss_call(31); }
static uint64_t gl_miss_32(void) { return gl_miss_call(32); }
static uint64_t gl_miss_33(void) { return gl_miss_call(33); }
static uint64_t gl_miss_34(void) { return gl_miss_call(34); }
static uint64_t gl_miss_35(void) { return gl_miss_call(35); }
static uint64_t gl_miss_36(void) { return gl_miss_call(36); }
static uint64_t gl_miss_37(void) { return gl_miss_call(37); }
static uint64_t gl_miss_38(void) { return gl_miss_call(38); }
static uint64_t gl_miss_39(void) { return gl_miss_call(39); }
static uint64_t gl_miss_40(void) { return gl_miss_call(40); }
static uint64_t gl_miss_41(void) { return gl_miss_call(41); }
static uint64_t gl_miss_42(void) { return gl_miss_call(42); }
static uint64_t gl_miss_43(void) { return gl_miss_call(43); }
static uint64_t gl_miss_44(void) { return gl_miss_call(44); }
static uint64_t gl_miss_45(void) { return gl_miss_call(45); }
static uint64_t gl_miss_46(void) { return gl_miss_call(46); }
static uint64_t gl_miss_47(void) { return gl_miss_call(47); }
static uint64_t gl_miss_48(void) { return gl_miss_call(48); }
static uint64_t gl_miss_49(void) { return gl_miss_call(49); }
static uint64_t gl_miss_50(void) { return gl_miss_call(50); }
static uint64_t gl_miss_51(void) { return gl_miss_call(51); }
static uint64_t gl_miss_52(void) { return gl_miss_call(52); }
static uint64_t gl_miss_53(void) { return gl_miss_call(53); }
static uint64_t gl_miss_54(void) { return gl_miss_call(54); }
static uint64_t gl_miss_55(void) { return gl_miss_call(55); }
static uint64_t gl_miss_56(void) { return gl_miss_call(56); }
static uint64_t gl_miss_57(void) { return gl_miss_call(57); }
static uint64_t gl_miss_58(void) { return gl_miss_call(58); }
static uint64_t gl_miss_59(void) { return gl_miss_call(59); }
static uint64_t gl_miss_60(void) { return gl_miss_call(60); }
static uint64_t gl_miss_61(void) { return gl_miss_call(61); }
static uint64_t gl_miss_62(void) { return gl_miss_call(62); }
static uint64_t gl_miss_63(void) { return gl_miss_call(63); }
static uint64_t gl_miss_64(void) { return gl_miss_call(64); }
static uint64_t gl_miss_65(void) { return gl_miss_call(65); }
static uint64_t gl_miss_66(void) { return gl_miss_call(66); }
static uint64_t gl_miss_67(void) { return gl_miss_call(67); }
static uint64_t gl_miss_68(void) { return gl_miss_call(68); }
static uint64_t gl_miss_69(void) { return gl_miss_call(69); }
static uint64_t gl_miss_70(void) { return gl_miss_call(70); }
static uint64_t gl_miss_71(void) { return gl_miss_call(71); }
static uint64_t gl_miss_72(void) { return gl_miss_call(72); }
static uint64_t gl_miss_73(void) { return gl_miss_call(73); }
static uint64_t gl_miss_74(void) { return gl_miss_call(74); }
static uint64_t gl_miss_75(void) { return gl_miss_call(75); }
static uint64_t gl_miss_76(void) { return gl_miss_call(76); }
static uint64_t gl_miss_77(void) { return gl_miss_call(77); }
static uint64_t gl_miss_78(void) { return gl_miss_call(78); }
static uint64_t gl_miss_79(void) { return gl_miss_call(79); }
static uint64_t gl_miss_80(void) { return gl_miss_call(80); }
static uint64_t gl_miss_81(void) { return gl_miss_call(81); }
static uint64_t gl_miss_82(void) { return gl_miss_call(82); }
static uint64_t gl_miss_83(void) { return gl_miss_call(83); }
static uint64_t gl_miss_84(void) { return gl_miss_call(84); }
static uint64_t gl_miss_85(void) { return gl_miss_call(85); }
static uint64_t gl_miss_86(void) { return gl_miss_call(86); }
static uint64_t gl_miss_87(void) { return gl_miss_call(87); }
static uint64_t gl_miss_88(void) { return gl_miss_call(88); }
static uint64_t gl_miss_89(void) { return gl_miss_call(89); }
static uint64_t gl_miss_90(void) { return gl_miss_call(90); }
static uint64_t gl_miss_91(void) { return gl_miss_call(91); }
static uint64_t gl_miss_92(void) { return gl_miss_call(92); }
static uint64_t gl_miss_93(void) { return gl_miss_call(93); }
static uint64_t gl_miss_94(void) { return gl_miss_call(94); }
static uint64_t gl_miss_95(void) { return gl_miss_call(95); }
static uint64_t gl_miss_96(void) { return gl_miss_call(96); }
static uint64_t gl_miss_97(void) { return gl_miss_call(97); }
static uint64_t gl_miss_98(void) { return gl_miss_call(98); }
static uint64_t gl_miss_99(void) { return gl_miss_call(99); }
static uint64_t gl_miss_100(void) { return gl_miss_call(100); }
static uint64_t gl_miss_101(void) { return gl_miss_call(101); }
static uint64_t gl_miss_102(void) { return gl_miss_call(102); }
static uint64_t gl_miss_103(void) { return gl_miss_call(103); }
static uint64_t gl_miss_104(void) { return gl_miss_call(104); }
static uint64_t gl_miss_105(void) { return gl_miss_call(105); }
static uint64_t gl_miss_106(void) { return gl_miss_call(106); }
static uint64_t gl_miss_107(void) { return gl_miss_call(107); }
static uint64_t gl_miss_108(void) { return gl_miss_call(108); }
static uint64_t gl_miss_109(void) { return gl_miss_call(109); }
static uint64_t gl_miss_110(void) { return gl_miss_call(110); }
static uint64_t gl_miss_111(void) { return gl_miss_call(111); }
static uint64_t gl_miss_112(void) { return gl_miss_call(112); }
static uint64_t gl_miss_113(void) { return gl_miss_call(113); }
static uint64_t gl_miss_114(void) { return gl_miss_call(114); }
static uint64_t gl_miss_115(void) { return gl_miss_call(115); }
static uint64_t gl_miss_116(void) { return gl_miss_call(116); }
static uint64_t gl_miss_117(void) { return gl_miss_call(117); }
static uint64_t gl_miss_118(void) { return gl_miss_call(118); }
static uint64_t gl_miss_119(void) { return gl_miss_call(119); }
static uint64_t gl_miss_120(void) { return gl_miss_call(120); }
static uint64_t gl_miss_121(void) { return gl_miss_call(121); }
static uint64_t gl_miss_122(void) { return gl_miss_call(122); }
static uint64_t gl_miss_123(void) { return gl_miss_call(123); }
static uint64_t gl_miss_124(void) { return gl_miss_call(124); }
static uint64_t gl_miss_125(void) { return gl_miss_call(125); }
static uint64_t gl_miss_126(void) { return gl_miss_call(126); }
static uint64_t gl_miss_127(void) { return gl_miss_call(127); }
static uint64_t gl_miss_128(void) { return gl_miss_call(128); }
static uint64_t gl_miss_129(void) { return gl_miss_call(129); }
static uint64_t gl_miss_130(void) { return gl_miss_call(130); }
static uint64_t gl_miss_131(void) { return gl_miss_call(131); }
static uint64_t gl_miss_132(void) { return gl_miss_call(132); }
static uint64_t gl_miss_133(void) { return gl_miss_call(133); }
static uint64_t gl_miss_134(void) { return gl_miss_call(134); }
static uint64_t gl_miss_135(void) { return gl_miss_call(135); }
static uint64_t gl_miss_136(void) { return gl_miss_call(136); }
static uint64_t gl_miss_137(void) { return gl_miss_call(137); }
static uint64_t gl_miss_138(void) { return gl_miss_call(138); }
static uint64_t gl_miss_139(void) { return gl_miss_call(139); }
static uint64_t gl_miss_140(void) { return gl_miss_call(140); }
static uint64_t gl_miss_141(void) { return gl_miss_call(141); }
static uint64_t gl_miss_142(void) { return gl_miss_call(142); }
static uint64_t gl_miss_143(void) { return gl_miss_call(143); }
static uint64_t gl_miss_144(void) { return gl_miss_call(144); }
static uint64_t gl_miss_145(void) { return gl_miss_call(145); }
static uint64_t gl_miss_146(void) { return gl_miss_call(146); }
static uint64_t gl_miss_147(void) { return gl_miss_call(147); }
static uint64_t gl_miss_148(void) { return gl_miss_call(148); }
static uint64_t gl_miss_149(void) { return gl_miss_call(149); }
static uint64_t gl_miss_150(void) { return gl_miss_call(150); }
static uint64_t gl_miss_151(void) { return gl_miss_call(151); }
static uint64_t gl_miss_152(void) { return gl_miss_call(152); }
static uint64_t gl_miss_153(void) { return gl_miss_call(153); }
static uint64_t gl_miss_154(void) { return gl_miss_call(154); }
static uint64_t gl_miss_155(void) { return gl_miss_call(155); }
static uint64_t gl_miss_156(void) { return gl_miss_call(156); }
static uint64_t gl_miss_157(void) { return gl_miss_call(157); }
static uint64_t gl_miss_158(void) { return gl_miss_call(158); }
static uint64_t gl_miss_159(void) { return gl_miss_call(159); }
static uint64_t gl_miss_160(void) { return gl_miss_call(160); }
static uint64_t gl_miss_161(void) { return gl_miss_call(161); }
static uint64_t gl_miss_162(void) { return gl_miss_call(162); }
static uint64_t gl_miss_163(void) { return gl_miss_call(163); }
static uint64_t gl_miss_164(void) { return gl_miss_call(164); }
static uint64_t gl_miss_165(void) { return gl_miss_call(165); }
static uint64_t gl_miss_166(void) { return gl_miss_call(166); }
static uint64_t gl_miss_167(void) { return gl_miss_call(167); }
static uint64_t gl_miss_168(void) { return gl_miss_call(168); }
static uint64_t gl_miss_169(void) { return gl_miss_call(169); }
static uint64_t gl_miss_170(void) { return gl_miss_call(170); }
static uint64_t gl_miss_171(void) { return gl_miss_call(171); }
static uint64_t gl_miss_172(void) { return gl_miss_call(172); }
static uint64_t gl_miss_173(void) { return gl_miss_call(173); }
static uint64_t gl_miss_174(void) { return gl_miss_call(174); }
static uint64_t gl_miss_175(void) { return gl_miss_call(175); }
static uint64_t gl_miss_176(void) { return gl_miss_call(176); }
static uint64_t gl_miss_177(void) { return gl_miss_call(177); }
static uint64_t gl_miss_178(void) { return gl_miss_call(178); }
static uint64_t gl_miss_179(void) { return gl_miss_call(179); }
static uint64_t gl_miss_180(void) { return gl_miss_call(180); }
static uint64_t gl_miss_181(void) { return gl_miss_call(181); }
static uint64_t gl_miss_182(void) { return gl_miss_call(182); }
static uint64_t gl_miss_183(void) { return gl_miss_call(183); }
static uint64_t gl_miss_184(void) { return gl_miss_call(184); }
static uint64_t gl_miss_185(void) { return gl_miss_call(185); }
static uint64_t gl_miss_186(void) { return gl_miss_call(186); }
static uint64_t gl_miss_187(void) { return gl_miss_call(187); }
static uint64_t gl_miss_188(void) { return gl_miss_call(188); }
static uint64_t gl_miss_189(void) { return gl_miss_call(189); }
static uint64_t gl_miss_190(void) { return gl_miss_call(190); }
static uint64_t gl_miss_191(void) { return gl_miss_call(191); }
static uint64_t gl_miss_192(void) { return gl_miss_call(192); }
static uint64_t gl_miss_193(void) { return gl_miss_call(193); }
static uint64_t gl_miss_194(void) { return gl_miss_call(194); }
static uint64_t gl_miss_195(void) { return gl_miss_call(195); }
static uint64_t gl_miss_196(void) { return gl_miss_call(196); }
static uint64_t gl_miss_197(void) { return gl_miss_call(197); }
static uint64_t gl_miss_198(void) { return gl_miss_call(198); }
static uint64_t gl_miss_199(void) { return gl_miss_call(199); }
static uint64_t gl_miss_200(void) { return gl_miss_call(200); }
static uint64_t gl_miss_201(void) { return gl_miss_call(201); }
static uint64_t gl_miss_202(void) { return gl_miss_call(202); }
static uint64_t gl_miss_203(void) { return gl_miss_call(203); }
static uint64_t gl_miss_204(void) { return gl_miss_call(204); }
static uint64_t gl_miss_205(void) { return gl_miss_call(205); }
static uint64_t gl_miss_206(void) { return gl_miss_call(206); }
static uint64_t gl_miss_207(void) { return gl_miss_call(207); }
static uint64_t gl_miss_208(void) { return gl_miss_call(208); }
static uint64_t gl_miss_209(void) { return gl_miss_call(209); }
static uint64_t gl_miss_210(void) { return gl_miss_call(210); }
static uint64_t gl_miss_211(void) { return gl_miss_call(211); }
static uint64_t gl_miss_212(void) { return gl_miss_call(212); }
static uint64_t gl_miss_213(void) { return gl_miss_call(213); }
static uint64_t gl_miss_214(void) { return gl_miss_call(214); }
static uint64_t gl_miss_215(void) { return gl_miss_call(215); }
static uint64_t gl_miss_216(void) { return gl_miss_call(216); }
static uint64_t gl_miss_217(void) { return gl_miss_call(217); }
static uint64_t gl_miss_218(void) { return gl_miss_call(218); }
static uint64_t gl_miss_219(void) { return gl_miss_call(219); }
static uint64_t gl_miss_220(void) { return gl_miss_call(220); }
static uint64_t gl_miss_221(void) { return gl_miss_call(221); }
static uint64_t gl_miss_222(void) { return gl_miss_call(222); }
static uint64_t gl_miss_223(void) { return gl_miss_call(223); }
static uint64_t gl_miss_224(void) { return gl_miss_call(224); }
static uint64_t gl_miss_225(void) { return gl_miss_call(225); }
static uint64_t gl_miss_226(void) { return gl_miss_call(226); }
static uint64_t gl_miss_227(void) { return gl_miss_call(227); }
static uint64_t gl_miss_228(void) { return gl_miss_call(228); }
static uint64_t gl_miss_229(void) { return gl_miss_call(229); }
static uint64_t gl_miss_230(void) { return gl_miss_call(230); }
static uint64_t gl_miss_231(void) { return gl_miss_call(231); }
static uint64_t gl_miss_232(void) { return gl_miss_call(232); }
static uint64_t gl_miss_233(void) { return gl_miss_call(233); }
static uint64_t gl_miss_234(void) { return gl_miss_call(234); }
static uint64_t gl_miss_235(void) { return gl_miss_call(235); }
static uint64_t gl_miss_236(void) { return gl_miss_call(236); }
static uint64_t gl_miss_237(void) { return gl_miss_call(237); }
static uint64_t gl_miss_238(void) { return gl_miss_call(238); }
static uint64_t gl_miss_239(void) { return gl_miss_call(239); }
static uint64_t gl_miss_240(void) { return gl_miss_call(240); }
static uint64_t gl_miss_241(void) { return gl_miss_call(241); }
static uint64_t gl_miss_242(void) { return gl_miss_call(242); }
static uint64_t gl_miss_243(void) { return gl_miss_call(243); }
static uint64_t gl_miss_244(void) { return gl_miss_call(244); }
static uint64_t gl_miss_245(void) { return gl_miss_call(245); }
static uint64_t gl_miss_246(void) { return gl_miss_call(246); }
static uint64_t gl_miss_247(void) { return gl_miss_call(247); }
static uint64_t gl_miss_248(void) { return gl_miss_call(248); }
static uint64_t gl_miss_249(void) { return gl_miss_call(249); }
static uint64_t gl_miss_250(void) { return gl_miss_call(250); }
static uint64_t gl_miss_251(void) { return gl_miss_call(251); }
static uint64_t gl_miss_252(void) { return gl_miss_call(252); }
static uint64_t gl_miss_253(void) { return gl_miss_call(253); }
static uint64_t gl_miss_254(void) { return gl_miss_call(254); }
static uint64_t gl_miss_255(void) { return gl_miss_call(255); }
static void *const g_gl_miss_fn[256] = {
    (void *)gl_miss_0, (void *)gl_miss_1, (void *)gl_miss_2, (void *)gl_miss_3, (void *)gl_miss_4, (void *)gl_miss_5, (void *)gl_miss_6, (void *)gl_miss_7,
    (void *)gl_miss_8, (void *)gl_miss_9, (void *)gl_miss_10, (void *)gl_miss_11, (void *)gl_miss_12, (void *)gl_miss_13, (void *)gl_miss_14, (void *)gl_miss_15,
    (void *)gl_miss_16, (void *)gl_miss_17, (void *)gl_miss_18, (void *)gl_miss_19, (void *)gl_miss_20, (void *)gl_miss_21, (void *)gl_miss_22, (void *)gl_miss_23,
    (void *)gl_miss_24, (void *)gl_miss_25, (void *)gl_miss_26, (void *)gl_miss_27, (void *)gl_miss_28, (void *)gl_miss_29, (void *)gl_miss_30, (void *)gl_miss_31,
    (void *)gl_miss_32, (void *)gl_miss_33, (void *)gl_miss_34, (void *)gl_miss_35, (void *)gl_miss_36, (void *)gl_miss_37, (void *)gl_miss_38, (void *)gl_miss_39,
    (void *)gl_miss_40, (void *)gl_miss_41, (void *)gl_miss_42, (void *)gl_miss_43, (void *)gl_miss_44, (void *)gl_miss_45, (void *)gl_miss_46, (void *)gl_miss_47,
    (void *)gl_miss_48, (void *)gl_miss_49, (void *)gl_miss_50, (void *)gl_miss_51, (void *)gl_miss_52, (void *)gl_miss_53, (void *)gl_miss_54, (void *)gl_miss_55,
    (void *)gl_miss_56, (void *)gl_miss_57, (void *)gl_miss_58, (void *)gl_miss_59, (void *)gl_miss_60, (void *)gl_miss_61, (void *)gl_miss_62, (void *)gl_miss_63,
    (void *)gl_miss_64, (void *)gl_miss_65, (void *)gl_miss_66, (void *)gl_miss_67, (void *)gl_miss_68, (void *)gl_miss_69, (void *)gl_miss_70, (void *)gl_miss_71,
    (void *)gl_miss_72, (void *)gl_miss_73, (void *)gl_miss_74, (void *)gl_miss_75, (void *)gl_miss_76, (void *)gl_miss_77, (void *)gl_miss_78, (void *)gl_miss_79,
    (void *)gl_miss_80, (void *)gl_miss_81, (void *)gl_miss_82, (void *)gl_miss_83, (void *)gl_miss_84, (void *)gl_miss_85, (void *)gl_miss_86, (void *)gl_miss_87,
    (void *)gl_miss_88, (void *)gl_miss_89, (void *)gl_miss_90, (void *)gl_miss_91, (void *)gl_miss_92, (void *)gl_miss_93, (void *)gl_miss_94, (void *)gl_miss_95,
    (void *)gl_miss_96, (void *)gl_miss_97, (void *)gl_miss_98, (void *)gl_miss_99, (void *)gl_miss_100, (void *)gl_miss_101, (void *)gl_miss_102, (void *)gl_miss_103,
    (void *)gl_miss_104, (void *)gl_miss_105, (void *)gl_miss_106, (void *)gl_miss_107, (void *)gl_miss_108, (void *)gl_miss_109, (void *)gl_miss_110, (void *)gl_miss_111,
    (void *)gl_miss_112, (void *)gl_miss_113, (void *)gl_miss_114, (void *)gl_miss_115, (void *)gl_miss_116, (void *)gl_miss_117, (void *)gl_miss_118, (void *)gl_miss_119,
    (void *)gl_miss_120, (void *)gl_miss_121, (void *)gl_miss_122, (void *)gl_miss_123, (void *)gl_miss_124, (void *)gl_miss_125, (void *)gl_miss_126, (void *)gl_miss_127,
    (void *)gl_miss_128, (void *)gl_miss_129, (void *)gl_miss_130, (void *)gl_miss_131, (void *)gl_miss_132, (void *)gl_miss_133, (void *)gl_miss_134, (void *)gl_miss_135,
    (void *)gl_miss_136, (void *)gl_miss_137, (void *)gl_miss_138, (void *)gl_miss_139, (void *)gl_miss_140, (void *)gl_miss_141, (void *)gl_miss_142, (void *)gl_miss_143,
    (void *)gl_miss_144, (void *)gl_miss_145, (void *)gl_miss_146, (void *)gl_miss_147, (void *)gl_miss_148, (void *)gl_miss_149, (void *)gl_miss_150, (void *)gl_miss_151,
    (void *)gl_miss_152, (void *)gl_miss_153, (void *)gl_miss_154, (void *)gl_miss_155, (void *)gl_miss_156, (void *)gl_miss_157, (void *)gl_miss_158, (void *)gl_miss_159,
    (void *)gl_miss_160, (void *)gl_miss_161, (void *)gl_miss_162, (void *)gl_miss_163, (void *)gl_miss_164, (void *)gl_miss_165, (void *)gl_miss_166, (void *)gl_miss_167,
    (void *)gl_miss_168, (void *)gl_miss_169, (void *)gl_miss_170, (void *)gl_miss_171, (void *)gl_miss_172, (void *)gl_miss_173, (void *)gl_miss_174, (void *)gl_miss_175,
    (void *)gl_miss_176, (void *)gl_miss_177, (void *)gl_miss_178, (void *)gl_miss_179, (void *)gl_miss_180, (void *)gl_miss_181, (void *)gl_miss_182, (void *)gl_miss_183,
    (void *)gl_miss_184, (void *)gl_miss_185, (void *)gl_miss_186, (void *)gl_miss_187, (void *)gl_miss_188, (void *)gl_miss_189, (void *)gl_miss_190, (void *)gl_miss_191,
    (void *)gl_miss_192, (void *)gl_miss_193, (void *)gl_miss_194, (void *)gl_miss_195, (void *)gl_miss_196, (void *)gl_miss_197, (void *)gl_miss_198, (void *)gl_miss_199,
    (void *)gl_miss_200, (void *)gl_miss_201, (void *)gl_miss_202, (void *)gl_miss_203, (void *)gl_miss_204, (void *)gl_miss_205, (void *)gl_miss_206, (void *)gl_miss_207,
    (void *)gl_miss_208, (void *)gl_miss_209, (void *)gl_miss_210, (void *)gl_miss_211, (void *)gl_miss_212, (void *)gl_miss_213, (void *)gl_miss_214, (void *)gl_miss_215,
    (void *)gl_miss_216, (void *)gl_miss_217, (void *)gl_miss_218, (void *)gl_miss_219, (void *)gl_miss_220, (void *)gl_miss_221, (void *)gl_miss_222, (void *)gl_miss_223,
    (void *)gl_miss_224, (void *)gl_miss_225, (void *)gl_miss_226, (void *)gl_miss_227, (void *)gl_miss_228, (void *)gl_miss_229, (void *)gl_miss_230, (void *)gl_miss_231,
    (void *)gl_miss_232, (void *)gl_miss_233, (void *)gl_miss_234, (void *)gl_miss_235, (void *)gl_miss_236, (void *)gl_miss_237, (void *)gl_miss_238, (void *)gl_miss_239,
    (void *)gl_miss_240, (void *)gl_miss_241, (void *)gl_miss_242, (void *)gl_miss_243, (void *)gl_miss_244, (void *)gl_miss_245, (void *)gl_miss_246, (void *)gl_miss_247,
    (void *)gl_miss_248, (void *)gl_miss_249, (void *)gl_miss_250, (void *)gl_miss_251, (void *)gl_miss_252, (void *)gl_miss_253, (void *)gl_miss_254, (void *)gl_miss_255,
};
void *gl_lookup(const char *name) {
    static const hle_ent *sec; static unsigned long sz;
    if (!name) return NULL;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swgl", &sz);
    void *fn = NULL;
    for (unsigned long i = 0; i < sz / sizeof *sec && !fn; i++) if (!strcmp(sec[i].name, name)) fn = sec[i].fn;
    static const char *seen[4096]; static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) { known = 1; break; }
    const char *how = fn ? "宿主实现" : "没有（回空）";
    if (!fn && gl_core_name(name)) {   // 同一个名字再查给同一个空实现
        static int nmiss; int i = 0; while (i < nmiss && strcmp(g_gl_miss_name[i], name)) i++;
        if (i == nmiss && nmiss < 256) { g_gl_miss_name[nmiss] = strdup(name); nmiss++; }
        if (i < nmiss) { fn = g_gl_miss_fn[i]; how = "没有（核心函数，交空实现）"; }
    }
    if (!known && nseen < 4096) { seen[nseen++] = strdup(name); GLOG("lookup %s → %s", name, how); }
    pthread_mutex_unlock(&mu);
    return fn;
}
void *sw_xstack_wrap(const char *name, void *fn);   // S2Arm 宿主：在宿主栈上跑的切栈桩（单测给替身）
int gl_is_big(const char *name);
// 交给游戏的入口（装载器解析的导入、eglGetProcAddress 交出的指针同一条路）：查表，重活（swglb 登记的）套切栈桩
void *gl_entry(const char *name) { void *fn = gl_lookup(name); return fn && gl_is_big(name) ? sw_xstack_wrap(name, fn) : fn; }
// __DATA,swglb：要跑在宿主栈上的名字（适配层交给游戏前套切栈桩）
int gl_is_big(const char *name) {
    static const char *const *sec; static unsigned long sz;
    if (!sec) sec = (const char *const *)getsectiondata(&_mh_execute_header, "__DATA", "swglb", &sz);
    for (unsigned long i = 0; sec && i < sz / sizeof *sec; i++) if (!strcmp(sec[i], name)) return 1;
    return 0;
}
