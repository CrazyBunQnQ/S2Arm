#include "nvn_vk_frame_stats.h"
#include "nvn_vk_timeline.h"

#include <android/log.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#define TAG "S2Arm"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// 近 16 帧窗口：一次尖峰不该决定读数，所以均值与峰值都给。
#define WIN_SAMPLES 16
static uint32_t g_win_frames;
static double g_win_gap_sum, g_win_gap_max, g_win_exec_sum, g_win_present_sum, g_win_cmds_sum;
// 同一窗口的逐帧 gap 样本：窗口固定 16 帧，够算真实的 p50/p95，不再拿窗口最大值冒充 p90。
static double g_win_gap_ms[WIN_SAMPLES];
static double g_last_present_s;
// other 的实测拆分：other 原本是 gap-exec-present 的残差，不是测量值。
// 这里用三个实测时间点把它拆成两段真实等待：
//   提交前(exec_end→present_start)：本帧命令录制完之后、上屏调用之前（我们自己的提交/收尾）
//   下次录制前(present_end→next_exec_begin)：上屏返回后到下一帧第一条命令（guest/节拍/调度）
static struct s2a_timeline g_tl;
// "上屏返回→下一帧首条命令"这段墙钟里，本线程实际消耗的 CPU 时间。
// 两者接近 = 线程在跑（guest CPU 模拟/逻辑就在这个线程上）；远小于墙钟 = 线程被换出或在等锁。
static double g_pe_cpu_s;          // 上屏返回时的线程 CPU 时间
static int g_pe_cpu_valid;
static pthread_t g_pe_cpu_tid;      // 记录 CPU 起点所属线程：跨线程比较线程 CPU 没有意义
static uint64_t g_win_next_cpu_cross;   // 因跨线程而丢弃的样本数（异步翻译模式下会出现）
static double g_win_next_cpu_sum;  // 该段内的线程 CPU 时间合计
static uint64_t g_win_next_cpu_n;   // 按 frame ID 配对的时间线（唯一权威；未测就输出"未测"）
static double g_exec_begin_s, g_exec_end_s, g_present_begin_s, g_present_end_s;
static double g_win_pre_submit_sum, g_win_pre_next_sum;
static int g_have_exec_begin, g_have_exec_end, g_have_present_end;

// 线程 CPU 时间（秒）；取不到返回 -1。
static double tl_thread_cpu_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return -1.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void nvn_vk_frame_stats_tl_mark(uint64_t frame_id, int kind, double now_s) {
    if (kind == S2A_TL_FIRST_CMD) {
        s2a_tl_note_next_first_cmd(&g_tl, now_s);
        if (g_pe_cpu_valid) {
            double cpu = tl_thread_cpu_s();
            // 只有同一线程才可比：异步翻译模式下 present 与下一帧首条命令在不同线程上，
            // 直接相减会得到几万毫秒的假数（实测 22839 ms）。
            if (cpu >= 0 && pthread_equal(pthread_self(), g_pe_cpu_tid)) {
                g_win_next_cpu_sum += cpu - g_pe_cpu_s;
                g_win_next_cpu_n++;
            } else {
                g_win_next_cpu_cross++;
            }
            g_pe_cpu_valid = 0;
        }
    } else if (kind == S2A_TL_PRESENT_END) {
        double cpu = tl_thread_cpu_s();
        if (cpu >= 0) { g_pe_cpu_s = cpu; g_pe_cpu_tid = pthread_self(); g_pe_cpu_valid = 1; }
    }
    (void)s2a_tl_mark(&g_tl, frame_id, kind, now_s);
}

void nvn_vk_frame_stats_tl_finish(uint64_t frame_id) { s2a_tl_finish(&g_tl, frame_id); }

void nvn_vk_frame_stats_exec_begin(double now_s) {
    // 新一帧开始录制：若上一帧的上屏已经结束，这段间隔就是"下次录制前"的真实等待。
    if (g_have_present_end && g_present_end_s > 0 && now_s > g_present_end_s)
        g_win_pre_next_sum += now_s - g_present_end_s;
    g_exec_begin_s = now_s;
    g_have_exec_begin = 1;
}

void nvn_vk_frame_stats_exec_end(double now_s) {
    g_exec_end_s = now_s;
    g_have_exec_end = 1;
}

void nvn_vk_frame_stats_present_span(double begin_s, double end_s) {
    g_present_begin_s = begin_s;
    g_present_end_s = end_s;
    g_have_present_end = 1;
    if (g_have_exec_end && g_have_exec_begin && begin_s >= g_exec_end_s)
        g_win_pre_submit_sum += begin_s - g_exec_end_s;
}

// W2.7 逐帧归因：编译/管线创建的累计计数（弱符号——没有绘制模块的构建里为 NULL）。
// 归因项是相邻两帧之间的增量，不是窗口累计。
void nvk_program_long_totals(uint64_t *compiles, uint64_t *compile_ns,
                             uint64_t *pipes, uint64_t *pipe_ns) __attribute__((weak));

// 长帧的既有出口：共享前端 hle_nvn.c 已经用 dm_longframe_note 报"整帧/提交+上屏/GPU/新管线/
// 纹理上传"；这里不另造一条通道，把编译侧的数字也送到同一个出口（没有该钩子时退回本模块日志）。
void dm_longframe_note(const char *what) __attribute__((weak));

void nvn_vk_frame_stats_present(uint64_t frame, double now_s, double exec_ms, double present_ms,
                                uint64_t commands) {
    // GPU 帧耗时（debug.s2a.gpu_time，默认关）：first_draw 在渲染 fence signal 后非阻塞读回
    // 帧首/帧尾 TOP-BOTTOM 时间戳（CPU 不等 GPU）；这里取「最新结算帧」并去重——同一帧只报一次，
    // 读不到就整段不输出（不写 0，对齐 iOS gpu_us 空口径）。
    extern int nvn_vk_first_draw_gpu_last(double *, int *, uint64_t *) __attribute__((weak));
    double gts_ms = 0.0; uint64_t gts_frame = 0;
    char gpu_seg[48] = {0};
    if (nvn_vk_first_draw_gpu_last && nvn_vk_first_draw_gpu_last(&gts_ms, NULL, &gts_frame)) {
        static uint64_t g_gpu_emitted;
        if (gts_frame != g_gpu_emitted) {
            g_gpu_emitted = gts_frame;
            snprintf(gpu_seg, sizeof gpu_seg, " gpu(f%llu)=%.2fms",
                     (unsigned long long)gts_frame, gts_ms);
        }
    }
    if (g_last_present_s > 0) {
        g_win_frames++;
        g_win_gap_sum += now_s - g_last_present_s;
        {
            double g = (now_s - g_last_present_s) * 1000.0;
            if (g > g_win_gap_max) g_win_gap_max = g;
            if (g_win_frames <= WIN_SAMPLES) g_win_gap_ms[g_win_frames - 1] = g;
        }
        g_win_exec_sum += exec_ms / 1000.0;
        g_win_present_sum += present_ms / 1000.0;
        g_win_cmds_sum += (double)commands;
    }

    if (nvk_program_long_totals && g_last_present_s > 0) {
        static uint64_t prev_c, prev_cns, prev_p, prev_pns;
        uint64_t c = 0, cns = 0, p = 0, pns = 0;
        nvk_program_long_totals(&c, &cns, &p, &pns);
        double gap_ms = (now_s - g_last_present_s) * 1000.0;
        double dc_ms = (double)(cns - prev_cns) / 1e6;
        double dp_ms = (double)(pns - prev_pns) / 1e6;
        if (gap_ms > 33.0) {
            // 编译可能跑在补编 worker 线程上，与 exec 重叠，所以只报"区间内"的墙钟，不从 gap 里减。
            // 等待/其它 = gap - exec（>0 才有意义）。
            double other_ms = gap_ms - exec_ms;
            char note[320];
            snprintf(note, sizeof note,
                     "frame %llu: gap %.1f ms, exec %.1f ms（区间内 编译 +%llu 次 %.1f ms、"
                     "管线创建 +%llu 次 %.1f ms）| 等待/其它 %.1f ms%s",
                     (unsigned long long)frame, gap_ms, exec_ms,
                     (unsigned long long)(c - prev_c), dc_ms,
                     (unsigned long long)(p - prev_p), dp_ms,
                     other_ms > 0.0 ? other_ms : 0.0, gpu_seg);
            if (dm_longframe_note) dm_longframe_note(note);
            else LOGI("longframe(本帧): %s", note);
        }
        prev_c = c; prev_cns = cns; prev_p = p; prev_pns = pns;
    }

    if (frame % 16 == 0 && g_last_present_s > 0) {
        double gap_ms = (now_s - g_last_present_s) * 1000.0;
        double wf = g_win_frames ? (double)g_win_frames : 1.0;
        double w_gap = g_win_gap_sum * 1000.0 / wf;
        double w_exec = g_win_exec_sum * 1000.0 / wf;
        double w_pres = g_win_present_sum * 1000.0 / wf;
        LOGI("frame timing(本帧): gap=%.1fms exec=%.1fms present=%.1fms other=%.1fms commands=%llu%s",
             gap_ms, exec_ms, present_ms, gap_ms - exec_ms - present_ms, (unsigned long long)commands,
             gpu_seg);
        {
            // 真实分位数（nearest-rank）：样本数就是窗口帧数，n 很小时 p95 会等于最大值——读取必须带 n，
            // 不能和更长的采样窗口直接比较。这里不再把窗口最大值写成 p90。
            uint32_t ns = g_win_frames < WIN_SAMPLES ? g_win_frames : WIN_SAMPLES;
            double sorted[WIN_SAMPLES];
            for (uint32_t i = 0; i < ns; i++) {
                double v = g_win_gap_ms[i];
                uint32_t j = i;
                while (j > 0 && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
                sorted[j] = v;
            }
            uint32_t i50 = ns ? (ns + 1) / 2 - 1 : 0;
            uint32_t i95 = ns ? (ns * 95 + 99) / 100 - 1 : 0;
            double p50 = ns ? sorted[i50] : 0.0;
            double p95 = ns ? sorted[i95 < ns ? i95 : ns - 1] : 0.0;
            LOGI("frame timing(近%u帧, frame=%llu): gap=%.1fms p50=%.1f p95=%.1f max=%.1f (n=%u) "
                 "exec=%.1fms present=%.1fms other=%.1fms[提交前=%.1f 下次录制前=%.1f] commands=%.0f fps=%.1f",
                 (unsigned)g_win_frames, (unsigned long long)frame, w_gap, p50, p95, g_win_gap_max,
                 (unsigned)ns, w_exec, w_pres, w_gap - w_exec - w_pres,
                 g_win_pre_submit_sum * 1000.0 / wf, g_win_pre_next_sum * 1000.0 / wf,
                 g_win_cmds_sum / wf, w_gap > 0 ? 1000.0 / w_gap : 0.0);
        {
            // 时间线：按 frame ID 配对的实测段；没有样本的段输出"未测"（不写 0）。
            static const int order[] = { S2A_TL_SEG_CMD_TO_DRAW, S2A_TL_SEG_DRAW_TO_END, S2A_TL_SEG_END_TO_PRESENT,
                                         S2A_TL_SEG_PRESENT, S2A_TL_SEG_PRESENT_TO_NEXT };
            char buf[512];
            size_t off = 0;
            for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++) {
                struct s2a_tl_stat st;
                int has = s2a_tl_take(&g_tl, order[i], &st);
                int n = snprintf(buf + off, sizeof buf - off, "%s%s=", i ? " " : "",
                                 s2a_tl_seg_name(order[i]));
                if (n > 0) off += (size_t)n;
                if (!has) {
                    n = snprintf(buf + off, sizeof buf - off, "未测");
                } else {
                    n = snprintf(buf + off, sizeof buf - off, "%.2f(%.0f%%,n=%llu,最大%.2f)",
                                 st.sum_ms / (double)st.n, 100.0 * st.sum_ms / (double)st.n / (w_gap > 0 ? w_gap : 1.0),
                                 (unsigned long long)st.n, st.max_ms);
                }
                if (n > 0) off += (size_t)n;
                if (off >= sizeof buf - 1) break;
            }
            LOGI("timeline(实测, 每帧均): %s | 上屏→下帧首命令 线程CPU=%s | 丢弃[重复=%llu 旧帧=%llu 缺样本=%llu 跨线程CPU=%llu]",
                 buf,
                 g_win_next_cpu_n ? ({ static char tmp[32]; snprintf(tmp, sizeof tmp, "%.2fms(n=%llu)",
                                      g_win_next_cpu_sum * 1000.0 / (double)g_win_next_cpu_n,
                                      (unsigned long long)g_win_next_cpu_n); tmp; }) : "未测",
                 (unsigned long long)g_tl.dup, (unsigned long long)g_tl.cross,
                 (unsigned long long)g_tl.unpaired, (unsigned long long)g_win_next_cpu_cross);
            g_win_next_cpu_sum = 0; g_win_next_cpu_n = 0; g_win_next_cpu_cross = 0;
            g_tl.dup = g_tl.cross = g_tl.unpaired = 0;
        }
        }
        g_win_pre_submit_sum = 0; g_win_pre_next_sum = 0;
        g_win_frames = 0; g_win_gap_sum = 0; g_win_gap_max = 0;
        g_win_exec_sum = 0; g_win_present_sum = 0; g_win_cmds_sum = 0;
    }

    g_last_present_s = now_s;
}
