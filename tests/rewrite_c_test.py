#!/usr/bin/env python3
"""Offline golden: corpus × Apple/Android × spin × raw/lz4; all emitted files against tests/fixtures/rewrite_golden/rewrite_c.tsv.

原为 C / Python 改写器对拍；10-31 删 Python 改写器后改对金样（见 tests/rewrite_golden.py）。用例名前缀参数只跑子集时不查金样表射程。
"""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests/rewrite_corpus'))
spec = importlib.util.spec_from_file_location('corpus', ROOT / 'tests/rewrite_corpus/run.py')
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)
sys.path.insert(0, str(ROOT / 'tests'))
from rewrite_golden import Golden

EXTRA_CASES = [
    dict(name='20_plain_stack_and_system', asm="""
      add x3, x18, #8
      adds w18, w18, w3
      str x18, [sp, #16]
      ldr x18, [sp, #8]
      ldur x18, [sp, #-8]
      stp x18, x2, [sp, #-16]!
      ldp x18, x2, [sp], #16
      stp x18, x2, [sp, #504]
      stp w18, w2, [sp, #252]
      ldpsw x18, x2, [sp, #252]
      stp x18, x2, [sp, #-96]!
      mov sp, x18
      ld1 {v31.16b}, [x18]
      mrs x16, tpidr_el0
      msr tpidr_el0, x16
      mrs x17, tpidrro_el0
      mrs x30, ctr_el0
      mrs x2, cntpct_el0
      mrs x3, cntvct_el0
      svc #32
      ret
    """),
    dict(name='21_cond_and_pc', asm="""
      adr x18, out
      cbz x18, out
      cbnz w18, out
      tbz x18, #12, out
      tbnz x18, #40, out
      adrp x18, 0x1000
out:  ret
    """),
    dict(name='22_sysreg_x18_gap', asm='mrs x18, tpidr_el0\nmsr tpidr_el0, x18\nret'),
    dict(name='23_system_whitelist_and_gaps', asm="""
      mrs x0, nzcv
      msr nzcv, x0
      mrs x1, dczid_el0
      dc cvau, x0
      dmb ish
      wfi
      mrs x1, cntfrq_el0
      brk #0xb123
      hlt #1
      ret
    """),
    dict(name='24_spin_rmw_balanced', asm="""
loop: ldxr w8, [x0]
      add w9, w8, #1
      stxr w10, w9, [x0]
      cbnz w10, loop
      ldr w11, [x1]
next: ldxr w8, [x0]
      sub w9, w8, #1
      stxr w10, w9, [x0]
      cbnz w10, next
      cbz w11, loop
      ret
    """),
    dict(name='25_spin_vector_views', asm="""
loop: ldr q31, [x0]
      mov v30.16b, v31.16b
      ldr w8, [x1]
      cbz w8, loop
      ret
    """),
    dict(name='26_widen_all_kinds', asm="""
      mov w3, #1280
      .inst 0xd2a89403
      .inst 0x5291c723
      .inst 0x72a7fc63
      fdiv s0, s1, s2
      str s3, [x0, #4]
      ldp s0, s1, [x0]
      ldp s2, s3, [x0, #8]
      ret
    """, widen=['main\t0x100\tmovz', 'main\t0x104\tf2w', 'main\t0x108\tasp',
                 'main\t0x110\taspdiv', 'main\t0x114\taspstr',
                 'main\t0x118\tlytsz', 'main\t0x11c\tcapsz', 'main\t0x2000\tf32']),
    dict(name='26b_widen_aspcap', asm="""
      .inst 0x52838e48
      .inst 0x72a800e8
      fmov s1, w8
      .inst 0x52838e49
      .inst 0x72a800e9
      str w9, [x8, #8]
      .inst 0x528aaaaa
      .inst 0x72a8010a
      ret
    """, widen=['main\t0x100\taspcap', 'main\t0x10c\taspcaps', 'main\t0x118\taspcap']),
    dict(name='27_widen_overlap_rejected', asm="""
      ldxr w0, [x1]
      add w18, w0, #1
      stxr w2, w18, [x1]
      cbnz w2, #-12
      mov w3, #1280
      ret
    """, widen=['main\t0x100\tmovz', 'main\t0x110\tmovz', 'main\t0x110\tmovz']),
    # Register names must be whole words, as in the Python oracle's \b patterns.
    # In particular, replacing x18 must leave #0x18 and #0x18a0 unchanged.
    dict(name='28_x18_hex_immediates', rc=0, need_x18=11, asm="""
      lsr w18, w18, #0x18
      str x17, [x18, #0x18]
      str w18, [x0, #0x18]
      str x18, [sp, #0x18]
      ldr x18, [x1, #0x18]
      ldur x18, [x2, #-0x18]
      add x18, x18, #0x18
      mov x18, #0x18
      mov w18, #0x18a
      eor w18, w18, #0x18
      cmp w18, #0x18
      ret
    """),
    dict(name='29_hex_immediates_without_x18', rc=0, need_x18=0, asm="""
      mov w0, #0x18b2
      mov x1, #0x1812
      mov w2, #0x18ff
      add x3, x4, #0x18
      str x5, [x6, #0x18]
      lsr w7, w8, #0x18
      brk #0x18
      ret
    """),
    # hlt 同 brk 放行（10-29 Minecraft 调试断点帮手）；hvc 照旧是缺口
    dict(name='29b_hlt_between_brks', rc=0, asm="""
      ldrb w8, [x0]
      cbnz w8, out
      brk #0
      hlt #0
      brk #0
out:  ret
    """),
    dict(name='29c_hvc_still_gap', gap_contains='异常类指令', asm='hvc #0\nret'),
    dict(name='30_hex_window_branch_target', text=0x2000, rc=0, need_x18=1, asm="""
      b loop
.org 0x17a0
loop: ldxr w8, [x0]
      add w8, w8, w18
      stxr w9, w8, [x0]
      cbnz w9, loop
      ret
    """),
    dict(name='31_x18_hex_window_branch_target', text=0x2000, rc=0, need_x18=1, asm="""
      b loop
.org 0x17a0
loop: ldxr w8, [x0]
      stxr w9, w8, [x0]
      cbnz w18, loop
      ret
    """),
    # Backedges occur as inner / outer / inner, unlike the sorted heads.
    # The inner region must keep both backedges, including the latest one.
    dict(name='32_spin_backedge_groups', rc=0,
         spin_sites=[(0x10c, [0x110, 0x118])], asm="""
outer: ldr w8, [x0]
      cbz w8, inner
      b done
inner: ldr w9, [x1]
      cbz w9, inner
      cbnz w8, outer
      cbnz w10, inner
done: ret
    """),
]

# Both callers must agree on all reaching values. The dead write makes linear
# recovery fail, so these exercise the CFG rule rather than the fast path.
for form in ('open', 'adr'):
    for variant, left, right, rc in (
        ('same', '.inst 0xb0000010\nadd x16, x16, #0x800',
         '.inst 0xb0000010\nadd x16, x16, #0x800', 0),
        ('different', '.inst 0xb0000010\nadd x16, x16, #0x804',
         '.inst 0xb0000010\nadd x16, x16, #0x800', 3),
        ('bad_definition', '.inst 0xb0000010\nmov x16, x4',
         '.inst 0xb0000010\nadd x16, x16, #0x800', 3),
        ('missing_page', 'mov x16, x4\nadd x16, x16, #0x800',
         '.inst 0xb0000010\nadd x16, x16, #0x800', 3),
        # Python ignores an undefined path when a concrete static value exists.
        ('partial_definition', 'nop\nnop',
         '.inst 0xb0000010\nadd x16, x16, #0x800', 0),
        ('undefined', 'nop\nnop', 'nop\nnop', 3),
    ):
        dispatch = ('ldrsw x18, [x16, x0, lsl #2]\nadd x18, x18, x16'
                    if form == 'open' else
                    'adr x18, dest\nldrsw x14, [x16, x0, lsl #2]\nadd x18, x18, x14')
        base = 0x1800 if form == 'open' else 0x180
        EXTRA_CASES.append(dict(
            name=f'33_cfg_{form}_{variant}', rc=rc,
            ro={0x800: ('i32', [x - base for x in (0x180, 0x188, 0x190)] + [0x7ffffff0])},
            br_targets={0x148 if form == 'open' else 0x14c: {0x180, 0x188, 0x190}} if rc == 0 else {},
            asm=f"""
      cbz w3, left
      {right}
      b merge
left: {left}
      b merge
      mov x16, x4
      ret
.org 0x40
merge: {dispatch}
      br x18
      ret
.org 0x80
dest: ret
      nop
      ret
      nop
      ret
"""))

EXTRA_CASES += [
    dict(name='34_asp_xd', rc=0, asm="""
      mov x12, #0x8e39
      movk x12, #0x3fe3, lsl #16
      mov x13, #0x8e3b
      movk x13, #0x3fe3, lsl #16
      ret
    """, widen=['main\t0x100\tasp', 'main\t0x108\tasp']),
    dict(name='35_asp_mixed_width', rc=0, asm="""
      mov w12, #0x8e39
      movk x12, #0x3fe3, lsl #16
      mov x13, #0x8e3b
      movk w13, #0x3fe3, lsl #16
      ret
    """, widen=['main\t0x100\tasp', 'main\t0x108\tasp']),
    dict(name='36_asp_different_register', rc=0, asm="""
      mov x12, #0x8e39
      movk x13, #0x3fe3, lsl #16
      ret
    """, widen=['main\t0x100\tasp']),
]

# Evaluate copy sources at the MOV; the current Python bound is sixteen copies.
_pages = '.inst 0xb000000c\nadd x12, x12, #0x800\n.inst 0xb0000001\nadd x1, x1, #0x800'
_four = 'mov x11, x12\nmov x10, x11\nmov x9, x10\nmov x16, x9'
_five = 'mov x11, x12\nmov x10, x11\nmov x9, x10\nmov x8, x9\nmov x16, x8'
for form in ('open', 'adr'):
    for variant, setup, right, left, rc, reason in (
        ('same_sources', _pages, 'mov x16, x12', 'mov x16, x1', 0, None),
        ('different_sources', _pages.replace('add x1, x1, #0x800', 'add x1, x1, #0x804'),
         'mov x16, x12', 'mov x16, x1', 3, '有 2 个不同的到达值（0x1800, 0x1804）'),
        ('unknown_source', _pages, 'mov x16, x12', 'mov x16, x4', 3, 'mov x16, x4：表基址 x4'),
        ('four_copies', _pages, _four, _four, 0, None),
        ('five_copies', _pages, _five, _five, 0, None),
        ('w_copy', _pages, 'mov w16, w12', 'mov w16, w1', 3, 'mov w16, w12 不是 add'),
        ('source_after_copy', _pages, 'mov x16, x12\nmov x12, x4', 'mov x16, x1', 0, None),
        ('cycle', _pages, 'copy_loop: mov x12, x16\nmov x16, x12\ncbnz w7, copy_loop',
         'mov x16, x1', 0, None),
    ):
        dispatch = ('ldrsw x18, [x16, x0, lsl #2]\nadd x18, x18, x16'
                    if form == 'open' else
                    'adr x18, dest\nldrsw x14, [x16, x0, lsl #2]\nadd x18, x18, x14')
        base = 0x1800 if form == 'open' else 0x1c0
        EXTRA_CASES.append(dict(
            name=f'37_copy_{form}_{variant}', rc=rc,
            ro={0x800: ('i32', [x - base for x in (0x1c0, 0x1c8, 0x1d0)] + [0x7ffffff0])},
            br_targets={0x188 if form == 'open' else 0x18c: {0x1c0, 0x1c8, 0x1d0}} if rc == 0 else {},
            gap_contains=reason if form == 'open' else None,
            asm=f"""
      {setup}
      cbz w3, left
      {right}
      b merge
left: {left}
      b merge
      mov x16, x4
      ret
.org 0x80
merge: {dispatch}
      br x18
      ret
.org 0xc0
dest: ret
      nop
      ret
      nop
      ret
"""))

# The bound counts new MOV definitions, not revisits of a copy cycle.
for form in ('open', 'adr'):
    for copies in (16, 17):
        registers = list(range(1, 18))[:copies - 1] + [16]
        chain = '\n'.join(f'mov x{dst}, x{src}' for dst, src in
                          zip(registers, [12] + registers[:-1]))
        dispatch = ('ldrsw x18, [x16, x0, lsl #2]\nadd x18, x18, x16'
                    if form == 'open' else
                    'adr x18, dest\nldrsw x14, [x16, x0, lsl #2]\nadd x18, x18, x14')
        base = 0x1800 if form == 'open' else 0x340
        EXTRA_CASES.append(dict(
            name=f'39_copy_{form}_{copies}_bound', rc=0 if copies == 16 else 3,
            ro={0x800: ('i32', [x - base for x in (0x340, 0x348, 0x350)] + [0x7ffffff0])},
            br_targets={0x308 if form == 'open' else 0x30c: {0x340, 0x348, 0x350}}
                       if copies == 16 else {},
            gap_contains='拷贝链超过 16 层' if copies == 17 and form == 'open' else None,
            asm=f"""
      .inst 0xb000000c
      add x12, x12, #0x800
      cbz w3, left
      {chain}
      b merge
left: {chain}
      b merge
      mov x16, x4
      ret
.org 0x200
merge: {dispatch}
      br x18
      ret
.org 0x240
dest: ret
      nop
      ret
      nop
      ret
"""))

# All 65 values must be reported in sorted order, beyond the old 600-byte gap
# buffer. Every path copies a separately defined static x12 into the base x16.
_many = '\n'.join(
    f'cbz w0, next{i}\n.inst 0xb000000c\nadd x12, x12, #{0x800 + 4*i}\n'
    f'mov x16, x12\nb merge\nnext{i}:' for i in range(64))
EXTRA_CASES.append(dict(
    name='38_copy_long_diagnostic', rc=3, gap_contains='有 65 个不同的到达值（',
    gap_min_bytes=600, asm=_many + """
      .inst 0xb000000c
      add x12, x12, #0x900
      mov x16, x12
      b merge
      mov x16, x4
      ret
merge: ldrsw x18, [x16, x0, lsl #2]
      add x18, x18, x16
      br x18
      ret
"""))

def shared_page_layout(native, golden, problems):
    """Expected offsets come from a 0x4000 image and each target's page grid."""
    with tempfile.TemporaryDirectory(prefix='rewrite-bias-') as tmp:
        temp = Path(tmp)
        source = temp / 'tiny.nso'
        corpus.nso([0xd65f03c0], {}, 4, str(source), 0x2000)
        data = bytearray(source.read_bytes())
        # Keep ro/data at 0x2000/0x3000 while text ends at 0xe10.
        # All three segments share a 16 KiB host page.
        struct.pack_into('<I', data, 0x18, 0xe10)
        struct.pack_into('<I', data, 0x60, 0xe10)
        source.write_bytes(data)
        apple = None
        for target in ('apple', 'ios', 'macos', '', 'android'):
            out = temp / f'{target or "default"}'
            out.mkdir()
            env = dict(os.environ, S2A_REWRITE_SPIN='0', S2A_SDK_ADDR_CUT='1')
            if target:
                env['S2A_TARGET'] = target
            else:
                env.pop('S2A_TARGET', None)
            result = subprocess.run([str(native), str(source), str(out), 'main'],
                env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, (target, result.stdout, result.stderr)
            output = {p.name: p.read_bytes() for p in out.iterdir()}
            assert set(output) == {'main.patched.bin', 'main.fixups.tsv',
                'main.gates.tsv', 'main.sites.tsv', 'main.gaps.tsv'}, (target, output.keys())
            header = dict(line[2:].split('\t', 1) for line in
                output['main.fixups.tsv'].decode().splitlines() if line.startswith('# '))
            android = target == 'android'
            expected = {'TEXT_END': 0xe10, 'RO_OFF': 0x2000, 'DATA_OFF': 0x3000,
                'KEY_PAGE_OFF': 0x4000 if android else 0x7000,
                'PATCH_OFF': 0x10000 if android else 0xf000, 'KEY_PAGE_SIZE': 0x4000}
            for key, value in expected.items():
                assert int(header[key], 16) == value, (target, key, header[key], hex(value))
            assert int(header.get('LOAD_BIAS', '0'), 16) == (0 if android else 0x1000), (target, header)
            if android:
                assert 'LOAD_BIAS' not in header, header
                assert header['TARGET'] == 'android', header
                assert header['REWRITE_ABI'] == 'android-sp-safe-v1', header
            else:
                assert 'TARGET' not in header and 'REWRITE_ABI' not in header, header
            # 全部文件、每个字节对金样（原为 C / Python 对拍）
            problems += [e for e in [golden.check(f'tiny-shared/{target or "default"}', result.returncode, source, out)] if e]
            if target != 'android':
                if apple is None:
                    apple = output
                assert output == apple, target
        # A large BSS triggers FAR without a large input file. FAR + a shared
        # text/data host page is unsupported on both targets, independently of
        # whether the target uses LOAD_BIAS. The rewriter may not emit an image.
        struct.pack_into('<I', data, 0x3c, 0x7000000)
        source.write_bytes(data)
        for target in ('apple', 'ios', 'macos', '', 'android'):
            out = temp / f'far-{target or "default"}'
            out.mkdir()
            env = dict(os.environ, S2A_REWRITE_SPIN='0', S2A_SDK_ADDR_CUT='1')
            if target:
                env['S2A_TARGET'] = target
            else:
                env.pop('S2A_TARGET', None)
            result = subprocess.run([str(native), str(source), str(out), 'main'],
                env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 1, ('FAR shared page', target,
                                           result.returncode, result.stdout, result.stderr)
            assert not (out / 'main.patched.bin').exists(), target
        print('FAR shared-page NSO with BSS=0x7000000: rejected on all targets, no image emitted')
        # Positive FAR control: move data to its own 16 KiB page, preserving
        # the same large BSS. Only the unsupported shared-page case is rejected.
        struct.pack_into('<I', data, 0x34, 0x4000)
        source.write_bytes(data)
        for target in ('apple', 'ios', 'macos', '', 'android'):
            out = temp / f'far-separate-{target or "default"}'
            out.mkdir()
            env = dict(os.environ, S2A_REWRITE_SPIN='0', S2A_SDK_ADDR_CUT='1')
            if target:
                env['S2A_TARGET'] = target
            else:
                env.pop('S2A_TARGET', None)
            result = subprocess.run([str(native), str(source), str(out), 'main'],
                env=env, capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, ('FAR separate pages', target,
                                           result.returncode, result.stdout, result.stderr)
            output = {p.name: p.read_bytes() for p in out.iterdir()}
            assert set(output) == {'main.patched.bin', 'main.fixups.tsv',
                'main.gates.tsv', 'main.sites.tsv', 'main.gaps.tsv'}, (target, output.keys())
            header = dict(line[2:].split('\t', 1) for line in
                output['main.fixups.tsv'].decode().splitlines() if line.startswith('# '))
            for key, expected in {'PATCH_OFF': -0x2000000, 'KEY_PAGE_OFF': 0x7008000,
                                  'FILE_IMAGE_LEN': 0x8000, 'IMAGE_SIZE': 0x700c000}.items():
                assert int(header[key], 16) == expected, (target, key, header[key])
            assert 'LOAD_BIAS' not in header, (target, header)
            problems += [e for e in [golden.check(f'far-separate/{target or "default"}', result.returncode, source, out)] if e]
        print('FAR separate-page control: succeed on all targets; full outputs checked against golden')

        print('tiny shared-page NSO: Android zero bias; Apple/default/aliases retain 0x1000; full bytes checked against golden')

# 单次改写的墙钟上限：只防卡死，不当性能判据。（当年 Python 改写器单跑合成 NSO 最慢约 10 秒，02_half_adr_ladder，本机 10-27），
#   列车在 gate-host 上并行跑满时被拖过旧的 60 秒，抛 TimeoutExpired、外层报成「产物不符」（10-26 两次红都是这个，产物没有差）
PER_CALL_S = 900

def main():
    native = Path(os.environ['REW']).resolve()
    corpus.REW = str(native)
    golden = Golden('rewrite_c'); problems = []
    elapsed = 0.0
    count = 0
    try:
        with tempfile.TemporaryDirectory(prefix='rewrite-c-') as tmp:
            temp = Path(tmp)
            # Some NSOs have unwind coverage over almost all of __text but no
            # per-function FDEs near the tail. A BL target must split that
            # pathological covered range through the existing fallback bounds.
            # Repeated LL/SC loads followed by a conditional branch expose the
            # old find_windows full-function predecessor scan.
            large_text = 32 * 1024 * 1024
            code = [0xD503201F] * ((0x400 - 0x100) // 4)
            code[0] = 0x94000040                  # bl 0x200
            code[(0x200 - 0x100) // 4] = 0xD53BD072  # mrs x18, tpidrro_el0
            code[(0x204 - 0x100) // 4] = 0x91000652  # add x18, x18, #1
            code[(0x208 - 0x100) // 4] = 0x9400003E  # bl 0x300
            code[(0x20C - 0x100) // 4] = 0xD65F03C0  # ret
            code[(0x300 - 0x100) // 4] = 0xD65F03C0  # ret
            # ldxr w8, [x0]; cbz w8, -4 (back to the ldxr), encoded directly
            # so this regression fixture also runs on Windows without xcrun.
            pair = [0x885F7C08, 0x34FFFFE8]
            code.extend(pair * 500_000)
            large_source = temp / 'large-covered-fde.nso'
            corpus.nso(code, {}, large_text - 0x100, str(large_source), large_text)
            # Correct the fixture's FDE initial-location pointer to a valid
            # pcrel __text address so the test exercises covered-function
            # boundaries rather than the uncovered-code fallback.
            image = bytearray(large_source.read_bytes())
            fde_pc_file_offset = 0x100 + large_text + 0x220
            struct.pack_into('<i', image, fde_pc_file_offset, 0x100 - (large_text + 0x220))
            large_source.write_bytes(image)
            large_out = temp / 'large-covered-fde-out'
            started = time.monotonic()
            rc, gaps, stat, log = corpus.rewrite(str(large_source), str(large_out), 'apple')
            large_elapsed = time.monotonic() - started
            if (rc or gaps or stat.get('tpidrro_r') != 1 or
                    not (large_out / 'main.patched.bin').is_file() or large_elapsed >= 20):
                raise AssertionError(f'large covered FDE fallback: rc={rc} gaps={gaps} stat={stat} elapsed={large_elapsed:.2f}s\n{log}')
            print(f'大范围 FDE 调用目标边界回退：32 MiB → rc=0、零缺口、{stat}，{large_elapsed:.3f}s')
            for case in corpus.CASES + EXTRA_CASES:
                if len(sys.argv) > 1 and not any(case["name"].startswith(p) for p in sys.argv[1:]):
                    continue
                code = corpus.assemble(case['name'], case['asm'])
                source = temp / (case['name'] + '.nso')
                wide = temp / 'widen.tsv'
                wide.write_text('\n'.join(case.get('widen', [])) + '\n')
                corpus.nso(code, case.get('ro', {}), case.get('fde', 4 * len(code)), str(source), case.get('text', 0x1000))
                # Exercise both compressed and uncompressed NSO segments.
                for compressed in (False, True):
                    selected = source
                    if compressed:
                        import lz4.block
                        data = source.read_bytes()
                        header = bytearray(data[:0x100]); payload = bytearray()
                        struct.pack_into('<I', header, 12, 7)
                        for k in range(3):
                            offset, _, size = struct.unpack_from('<III', header, 0x10 + 16*k)
                            block = lz4.block.compress(data[offset:offset+size], store_size=False)
                            struct.pack_into('<I', header, 0x10+16*k, 0x100+len(payload))
                            struct.pack_into('<I', header, 0x60+4*k, len(block))
                            payload += block
                        selected = temp / 'compressed.nso'; selected.write_bytes(header + payload)
                    for target in ('apple', 'android'):
                        for spin in (0, 1):
                            env = dict(os.environ, S2A_TARGET=target, S2A_REWRITE_SPIN=str(spin))
                            label = f"{case['name']} {target} spin={spin} compressed={compressed}"
                            out = temp / 'out'; out.mkdir(exist_ok=True)
                            for file in out.iterdir(): file.unlink()
                            start = time.perf_counter()
                            try:
                                result = subprocess.run([str(native), str(selected), str(out), 'main'] + ([str(wide)] if 'widen' in case else []), env=env, capture_output=True, text=True, timeout=PER_CALL_S)
                            except subprocess.TimeoutExpired:
                                sys.exit(f"🔴 超时（不是产物差）：{label} 改写超过 {PER_CALL_S} 秒")
                            elapsed += time.perf_counter() - start
                            output = {p.name: p.read_bytes() for p in out.iterdir()}
                            if 'rc' in case:
                                assert result.returncode == case['rc'], (
                                    label, result.stdout, result.stderr)
                            if 'need_x18' in case:
                                needs = next(
                                    line.split('\t', 1)[1]
                                    for line in output['main.gaps.tsv'].decode().splitlines()
                                    if line.startswith('# 需求分类\t'))
                                needs = dict(item.split('=') for item in needs.split())
                                assert int(needs.get('x18', 0)) == case['need_x18'], (label, needs)
                            # 退出码与全部文件每个字节对金样（原为 C / Python 对拍）
                            problems += [e for e in [golden.check(f"{case['name']}/{target}/spin{spin}/{'lz4' if compressed else 'raw'}", result.returncode, selected, out)] if e]
                            if case.get('gap_contains'):
                                gaps = [line for line in output['main.gaps.tsv'].decode().splitlines()
                                        if line and not line.startswith('#')]
                                assert len(gaps) == 1 and case['gap_contains'] in gaps[0], (label, gaps)
                                assert len(gaps[0].encode()) > case.get('gap_min_bytes', 0), (label, gaps)
                            if case.get('br_targets'):
                                landings = corpus.landings(str(out), 4 * len(code))
                                for site, expected in case['br_targets'].items():
                                    assert landings[site] == (expected, False), (label, landings, expected)
                            if 'spin_sites' in case:
                                expected = case['spin_sites'] if spin else []
                                actual = []
                                for line in output['main.sites.tsv'].decode().splitlines():
                                    fields = line.split('\t')
                                    if fields[1] == 'S':
                                        actual.append((int(fields[0], 16),
                                                       [int(x, 16) for x in fields[2].split()]))
                                assert actual == expected, (label, actual, expected)
                            count += 1
                print('已对金样', case['name'])
        shared_page_layout(native, golden, problems)
        problems += golden.finish(complete=len(sys.argv) == 1)
        if problems:
            sys.exit('🔴 产物与金样不符（有意改了改写产物就按 tests/rewrite_golden.py 说明重写金样并在提交里写明）：\n' + '\n'.join(problems))
        print(f'合成 NSO 金样：{count} 组 + 页布局 10 组全同；C {elapsed:.3f}s（包含进程启动，不代表九款耗时）')
    finally:
        shutil.rmtree(corpus.T)

if __name__ == '__main__':
    main()
