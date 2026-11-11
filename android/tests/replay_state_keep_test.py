#!/usr/bin/env python3
"""mtl_sub_begin 的生产函数体：回放状态只在本进程第一次提交前复位（#539）。

NVN 的绑定状态挂在队列上，上一批命令设的池 / 程序下一批仍然有效（Metal nvn_be_exec 只在首次复位）。
每次提交都复位时，游戏只在早先的提交里设过一次的采样器池到首笔 draw 已是 0，编译请求被拒。
宿主契约测试：只数「复位作业入队几次」，不碰 Vulkan。--source-ref 取旧版本作负对照。
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source-ref', help='compile the shim body at this git revision')
parser.add_argument('--expect-resets', type=int, default=1)
args = parser.parse_args()
path = 'android/native/nvn/mtl_vk_shim.c'
text = subprocess.check_output(['git', 'show', f'{args.source_ref}:{path}'], cwd=root, text=True) if args.source_ref else (root / path).read_text()
start = text.index('void mtl_sub_begin(void)')
pos = text.index('{', start) + 1
depth = 1
while depth:
    depth += (text[pos] == '{') - (text[pos] == '}')
    pos += 1
body = text[start:pos]
source = r'''
#include <stdio.h>
#include <stdarg.h>
enum { SHIM_ASYNC_OFF, SHIM_ASYNC_CONSERVATIVE };
static int g_sub_active, resets, flat_resets, drains, reds;
static void nvk_require(void) {}
static void red(const char *format, ...) { (void)format; reds++; }
static int shim_async_mode(void) { return SHIM_ASYNC_CONSERVATIVE; }
static void shim_drain(void) { drains++; }
static void job_reset_state(void *arg) { (void)arg; }
static void job_other(void *arg) { (void)arg; }
static void shim_enqueue(void (*fn)(void *), void *arg) { (void)arg; if (fn == job_reset_state) resets++; (void)job_other; }
static void s2a_record_flat_reset(void) { flat_resets++; }
''' + body + r'''
int main(void) {
    for (int i = 0; i < 3; i++) { mtl_sub_begin(); g_sub_active = 0; }   // 三次提交
    printf("resets=%d flat_resets=%d drains=%d reds=%d\n", resets, flat_resets, drains, reds);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp) / 't.c', Path(tmp) / 't'
    src.write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    out = subprocess.check_output([str(exe)], text=True).strip()
# 三次提交：每次提交的扁平记录缓冲照旧各复位一次、保守档照旧各排空一次；绑定状态只复位 expect_resets 次
expected = f'resets={args.expect_resets} flat_resets=3 drains=3 reds=0'
print(f'replay state keep: 期望 {expected} → 真结果 {out}')
sys.exit(0 if out == expected else 1)
