// 直连安全网：连续起局失败计数（接口与口径见 runtime/include/s2a_dguard.h）。
// 记录文件 <Documents>/wire/direct_guard（文本，整份先写临时文件再改名）：
//   第 1 行 「s2a_dguard 1 <程序号 16 位十六进制> <出包指纹> <宿主提交号>」（宿主提交号缺 = 旧记录，当作「-」：与任何有提交号的宿主都不同 → 从 0 数）
//   之后每行一次失败的阶段，旧的在前；本次起局 arm 之后，最后一行就是本次（随首帧 / 每 10 秒改写）。
//   行首「?」= 待定：这局还没有失败证据。下次起局读到待定行就丢掉（被外部结束 / 玩家退出都不算直连失败），只数有证据的行
#include "native.h"
#include "s2a_dguard.h"
#include "s2a_docs.h"   // 文档根唯一取法
#include "s2a_unattended.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#else
#include <sys/utsname.h>
#endif

#define MAX_ROWS 16   // 只留最近这么多次（提示只看是否 ≥ 3，诊断里列出来）
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_live;           // 已 arm、还没清零：frame / background 才干活
static uint64_t g_pid; static char g_pack[24], g_host[48];
static char g_rows[MAX_ROWS][48]; static int g_nrows;   // 此前的失败（不含本次）
static int g_cur_in;                 // 本次那一行在不在文件里（arm 后 = 在，清零后不在）
static char g_cur[48];               // 本次的阶段
static char g_why[24];               // 本次的失败证据（崩溃 / 内存告警 / 画面停住）；空 = 待定
static int g_why_stall;              // 证据是画面停住：帧又走了撤掉、进后台不撤
static uint64_t g_t0, g_acc;         // 首帧后：本段前台起点、之前各段前台累计（纳秒）
static int g_first, g_fg, g_bucket;

static void docs_path(char *out, size_t n, const char *name) {
    s2a_docs_path(out, n, "wire%s%s", name ? "/" : "", name ? name : "");
}
// 读记录：返回失败行数（≥ 0），文件不在 / 头不对 = 0；pid / pack 给了就填（头里的值）
static int load(uint64_t *pid, char *pack, size_t pn, char *host, size_t hn, char rows[][48], int max) {
    char p[1200], line[256]; docs_path(p, sizeof p, "direct_guard");
    FILE *f = fopen(p, "r"); if (!f) return 0;
    int n = 0, dropped = 0; unsigned long long id = 0; char pk[64] = "", hs[64] = "-";
    if (!fgets(line, sizeof line, f) || sscanf(line, "s2a_dguard 1 %16llx %63s %63s", &id, pk, hs) < 2) { fclose(f); return 0; }
    if (pid) *pid = id;
    if (pack) snprintf(pack, pn, "%s", pk);
    if (host) snprintf(host, hn, "%s", hs);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; if (!line[0]) continue;
        if (line[0] == '?') { dropped++; continue; }   // 待定行：那局没留下失败证据
        if (n < max) snprintf(rows[n], 48, "%.47s", line); else { memmove(rows, rows + 1, (size_t)(max - 1) * 48); snprintf(rows[max - 1], 48, "%.47s", line); }   // 显式精度：与按 48 字节截断逐字节相同，gcc -Wformat-truncation 不再报
        if (n < max) n++;
    }
    fclose(f);
    if (dropped && pid) LOG("直连安全网：上次起局 %d 次没有失败证据（被外部结束 / 玩家退出），不计", dropped);
    return n;
}
static void save(void) {   // 持锁调：头 + 此前的失败 + 本次（在的话）
    char d[1200], p[1300], t[1400]; docs_path(d, sizeof d, NULL);
    if (mkdir(d, 0755) && errno != EEXIST) { LOG("【红】直连安全网：建 %s 失败（errno %d）", d, errno); return; }
    snprintf(p, sizeof p, "%s/direct_guard", d); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) { LOG("【红】直连安全网：写 %s 失败（errno %d）", t, errno); return; }
    fprintf(f, "s2a_dguard 1 %016llX %s %s\n", (unsigned long long)g_pid, g_pack, g_host);
    for (int i = 0; i < g_nrows; i++) fprintf(f, "%s\n", g_rows[i]);
    if (g_cur_in) { if (g_why[0]) fprintf(f, "%s（%s）\n", g_cur, g_why); else fprintf(f, "?%s\n", g_cur); }
    fflush(f); fsync(fileno(f));   // 本次之后可能马上崩：落到盘上再往下走
    if (fclose(f) || rename(t, p)) LOG("【红】直连安全网：写 %s 失败（errno %d）", p, errno);
}

int s2a_dguard_needs_ui(void) { char rows[MAX_ROWS][48]; return !s2a_unattended() && load(NULL, NULL, 0, NULL, 0, rows, MAX_ROWS) >= S2A_DGUARD_LIMIT; }   // 自动化局不起界面：host_main.c 记日志收局

const char *s2a_dguard_pack_fp(const char *gd) {
    static char fp[24]; snprintf(fp, sizeof fp, "-");
    if (!gd || !*gd) return fp;
    char p[1200]; snprintf(p, sizeof p, "%s/manifest.json", gd);
    FILE *f = fopen(p, "r"); if (!f) return fp;
    char buf[8192]; size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
    char *k = strstr(buf, "\"index_sha256\""); if (!k) return fp;
    k = strchr(k + 14, '"'); if (!k) return fp;
    k++; int i = 0; while (i < 16 && isxdigit((unsigned char)k[i])) i++;
    if (i == 16) { memcpy(fp, k, 16); fp[16] = 0; }
    return fp;
}

int s2a_dguard_begin(uint64_t pid, const char *pack, const char *host) {
    pthread_mutex_lock(&g_mu);
    uint64_t fpid = 0; char fpack[64] = "", fhost[64] = "";
    g_nrows = load(&fpid, fpack, sizeof fpack, fhost, sizeof fhost, g_rows, MAX_ROWS);
    g_pid = pid; snprintf(g_pack, sizeof g_pack, "%s", pack && *pack ? pack : "-"); snprintf(g_host, sizeof g_host, "%s", host && *host ? host : "-");
    // 包身份 = 程序号 + 出包指纹 + 宿主提交号：只更新宿主（着色器包没变）的新版本也从 0 数，不被旧宿主的失败拦在提示页
    if (g_nrows && (fpid != pid || strcmp(fpack, g_pack) || strcmp(fhost, g_host))) {
        LOG("直连安全网：记录是另一个包（程序号 %016llX、出包指纹 %s、宿主 %s），本包 %016llX / %s / %s 从 0 数", (unsigned long long)fpid, fpack, fhost, (unsigned long long)pid, g_pack, g_host);
        g_nrows = 0;
    }
    int n = g_nrows;
    pthread_mutex_unlock(&g_mu);
    LOG("直连安全网：此前连续失败 %d 次（满 %d 次起局前提示）", n, S2A_DGUARD_LIMIT);
    return n;
}

void s2a_dguard_arm(uint64_t now) {
    (void)now;
    pthread_mutex_lock(&g_mu);
    if (g_nrows == MAX_ROWS) { memmove(g_rows, g_rows + 1, (MAX_ROWS - 1) * 48); g_nrows--; }
    snprintf(g_cur, sizeof g_cur, "首帧前"); g_cur_in = 1; g_why[0] = 0; g_why_stall = 0; g_fg = 1; g_first = 0; g_acc = 0; g_bucket = -1;
    save(); atomic_store(&g_live, 1);
    pthread_mutex_unlock(&g_mu);
}

static void stage_after(int b) { snprintf(g_cur, sizeof g_cur, "首帧后 %d–%d 秒", b * 10, b * 10 + 10); }
void s2a_dguard_frame(uint64_t now) {
    if (!atomic_load_explicit(&g_live, memory_order_acquire)) return;
    pthread_mutex_lock(&g_mu);
    if (!atomic_load(&g_live) || !g_fg) { pthread_mutex_unlock(&g_mu); return; }
    if (!g_first) { g_first = 1; g_t0 = now; g_bucket = 0; stage_after(0); save(); pthread_mutex_unlock(&g_mu); return; }
    uint64_t e = g_acc + (now > g_t0 ? now - g_t0 : 0);
    if (e >= S2A_DGUARD_OK_NS) {
        g_nrows = 0; g_cur_in = 0; save(); atomic_store(&g_live, 0);
        pthread_mutex_unlock(&g_mu);
        LOG("直连安全网：首帧后前台已满 %llu 秒，连续失败清零", (unsigned long long)(S2A_DGUARD_OK_NS / 1000000000ull));
        return;
    }
    int b = (int)(e / 10000000000ull);
    if (b != g_bucket) { g_bucket = b; stage_after(b); save(); }
    pthread_mutex_unlock(&g_mu);
}

void s2a_dguard_background(int on, uint64_t now) {
    if (!atomic_load(&g_live)) return;
    pthread_mutex_lock(&g_mu);
    if (atomic_load(&g_live)) {
        // 进后台：停表；证据撤回待定（内存告警之后在后台被回收不算），画面停住之后玩家切走去划掉照算
        if (on && g_fg) { if (g_first && now > g_t0) g_acc += now - g_t0; g_fg = 0; if (g_why[0] && !g_why_stall) { g_why[0] = 0; save(); } }
        else if (!on && !g_fg) { g_fg = 1; g_t0 = now; }
    }
    pthread_mutex_unlock(&g_mu);
}

// 失败证据：why 非空 = 记上（崩溃处理、前台内存告警、出图停住看门），NULL = 撤掉画面停住（帧又走了）。没 arm / 已清零时空操作。
//   崩溃处理里调：锁可能正被崩掉的线程持着，拿不到锁就不加锁直接写（进程马上没了，不会再有别的写者）
void s2a_dguard_evidence(const char *why, int stall) {
    if (!atomic_load(&g_live)) return;
    int crash = why && !strcmp(why, "崩溃"), locked = !pthread_mutex_trylock(&g_mu);
    if (!locked && !crash) { pthread_mutex_lock(&g_mu); locked = 1; }
    if (atomic_load(&g_live)) {
        if (why && (!g_why[0] || g_why_stall)) { snprintf(g_why, sizeof g_why, "%s", why); g_why_stall = stall; save(); }
        else if (!why && g_why_stall) { g_why[0] = 0; g_why_stall = 0; save(); }
    }
    if (locked) pthread_mutex_unlock(&g_mu);
}

size_t s2a_dguard_diag(char *out, size_t n, const char *build_id, const char *chain) {
    char model[64] = "?", osv[64] = "?";
#if defined(__APPLE__)
    size_t l = sizeof model; if (sysctlbyname(TARGET_OS_IPHONE ? "hw.machine" : "hw.model", model, &l, NULL, 0)) snprintf(model, sizeof model, "?");   // Mac：hw.machine 是 arm64，型号在 hw.model
    l = sizeof osv; if (sysctlbyname("kern.osproductversion", osv, &l, NULL, 0)) snprintf(osv, sizeof osv, "?");
#else
    struct utsname u; if (!uname(&u)) { snprintf(model, sizeof model, "%.63s", u.machine); snprintf(osv, sizeof osv, "%.31s %.31s", u.sysname, u.release); }
#endif
    pthread_mutex_lock(&g_mu);
    uint64_t pid = g_pid; char pack[24], host[48]; snprintf(pack, sizeof pack, "%s", g_pack); snprintf(host, sizeof host, "%s", g_host);
    char rows[MAX_ROWS][48]; int nr = g_nrows; memcpy(rows, g_rows, sizeof rows);
    pthread_mutex_unlock(&g_mu);
    size_t w = (size_t)snprintf(out, n, "S2Arm 直连安全网诊断\n程序号 %016llX\n主程序构建号 %s\n出包指纹 %s\n宿主提交号 %s\n翻译链 %s\n设备 %s\n系统 %s\n连续失败 %d 次：\n",
        (unsigned long long)pid, build_id && *build_id ? build_id : "-", pack, host, chain && *chain ? chain : "-", model, osv, nr);
    for (int i = 0; i < nr && w < n; i++) w += (size_t)snprintf(out + w, n - w, "  第 %d 次：%s\n", i + 1, rows[i]);
    return w < n ? w : (n ? n - 1 : 0);
}
