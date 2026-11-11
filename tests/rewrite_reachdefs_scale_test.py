#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""CFG table-base recovery must visit only actual predecessors, not rescan the function.

REW=<C rewriter> python3 tests/rewrite_reachdefs_scale_test.py
The synthetic NSO repeats an equivalent diamond before each indirect branch.
Each branch uses jt_open -> jt_base -> reg_const -> reachdefs, with both paths
agreeing on the same table address. The expected table-open count is known from
the fixture; no game-derived bytes are used.

Gate coverage: tests/rewrite_c_gate_test.sh
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

REW = os.environ["REW"]
N = int(os.environ.get("S2A_SCALE_N", "3000"))
LIMIT = 15
RET = 0xD65F03C0


def adrp(pc, target, rd):
    pages = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    imm = pages & 0x1FFFFF
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


def b(pc, target):
    return 0x14000000 | (((target - pc) >> 2) & 0x3FFFFFF)


def cbz_w(pc, target, rt):
    return 0x34000000 | ((((target - pc) >> 2) & 0x7FFFF) << 5) | rt


def bhi(pc, target):
    return 0x54000008 | ((((target - pc) >> 2) & 0x7FFFF) << 5)


def ldrsw_reg(rd, rn, rm):
    return (0xB8A07800 | (rm << 16) | (3 << 13) | (1 << 12) | (rn << 5) | rd)


def nso(path, n):
    block_words = 11
    code_bytes = 0x100 + 4 * (block_words * n + 1)
    text_size = (code_bytes + 0xFFF) & ~0xFFF
    table = text_size + 0x800
    end_ret = 0x100 + 4 * block_words * n
    code = []
    for i in range(n):
        p = 0x100 + 4 * block_words * i
        code.extend((
            adrp(p, table, 12),
            0x91000000 | (0x800 << 10) | (12 << 5) | 12,  # add x12, x12, #0x800
            cbz_w(p + 8, p + 20, 0),                       # cbz w0, alternate write
            0xAA0C03F0,                                     # mov x16, x12
            b(p + 16, p + 24),                             # b join
            0xAA0C03F0,                                     # mov x16, x12
            0x710005DF,                                     # cmp w14, #1
            bhi(p + 28, end_ret),                           # b.hi shared return
            ldrsw_reg(14, 16, 14),                          # ldrsw x14, [x16, x14, lsl #2]
            0x8B1001D2,                                     # add x18, x14, x16
            0xD61F0240,                                     # br x18
        ))
    code.append(RET)

    ro_size = 0x2000
    eh, ef = 0x100, 0x120
    ro = bytearray(ro_size)
    struct.pack_into("<4sIIIii", ro, 0, b"MOD0", 0, 0, 0, eh, eh + 0x20)
    struct.pack_into("<BBBBiI", ro, eh, 1, 0x1B, 0x03, 0x3B,
                     (text_size + ef) - (text_size + eh + 4), 1)
    fde = text_size + ef + 0x18
    struct.pack_into("<ii", ro, eh + 12, 0x100 - (text_size + eh),
                     fde - (text_size + eh))
    ro[ef:ef + 0x18] = struct.pack("<II", 0x14, 0) + bytes([1]) + b"zR\0" + bytes([4, 0x78, 30, 1, 0x1B]) + bytes(7)
    struct.pack_into("<IIii", ro, ef + 0x18, 0x0C, (fde + 4) - (text_size + ef),
                     0x100 - (fde + 8), 4 * len(code))
    struct.pack_into("<ii", ro, 0x800, end_ret - table, end_ret - table)
    struct.pack_into("<i", ro, 0x808, 0)

    text = bytearray(text_size)
    struct.pack_into("<II", text, 0, 0x14000040, text_size)
    struct.pack_into(f"<{len(code)}I", text, 0x100, *code)
    data = bytes(0x1000)
    segs = ((0, bytes(text)), (text_size, bytes(ro)), (text_size + ro_size, data))
    header = bytearray(0x100)
    header[:4] = b"NSO0"
    for k, (mem_off, segment) in enumerate(segs):
        file_off = 0x100 + sum(len(s[1]) for s in segs[:k])
        struct.pack_into("<III", header, 0x10 + 0x10 * k, file_off, mem_off, len(segment))
        struct.pack_into("<I", header, 0x60 + 4 * k, len(segment))
    header[0x40:0x50] = bytes(range(16))
    with open(path, "wb") as f:
        f.write(header)
        for _, segment in segs:
            f.write(segment)


with tempfile.TemporaryDirectory() as tmp:
    source = os.path.join(tmp, "main")
    out = os.path.join(tmp, "out")
    os.mkdir(out)
    nso(source, N)
    started = time.monotonic()
    result = subprocess.run([REW, source, out, "main"], capture_output=True,
                            encoding="utf-8", errors="replace", timeout=600,
                            env=dict(os.environ, S2A_TARGET="ios"))
    elapsed = time.monotonic() - started
    text = result.stdout + result.stderr
    match = re.search(r"PFRW mod=main need=(\d+) gaps=(\d+)", text)
    opened = re.search(r"'br_tab_open': (\d+)", text)
    expected_opened = int(opened.group(1)) if opened else None
    ok = result.returncode == 0 and match and int(match.group(2)) == 0 and expected_opened == N
    print(f"{'通' if ok else '不通'} {N} 个 CFG 基址跳转表：期望退出 0 / 零缺口 / br_tab_open={N}；"
          f"真结果 rc={result.returncode} / {match.groups() if match else '无 PFRW'} / "
          f"br_tab_open={expected_opened} / {elapsed:.2f}s")
    fast = elapsed < LIMIT
    print(f"{'通' if fast else '不通'} {LIMIT} 秒内完成：期望 True → 真结果 {fast}")
    if not ok or not fast:
        if not match or not ok:
            print("\n".join(text.splitlines()[-12:]))
        sys.exit(1)
