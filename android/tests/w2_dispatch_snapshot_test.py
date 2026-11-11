#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Compile real vk_record on Linux; Vulkan submit/fence calls are host mocks.
Requires host cc, libvulkan.so.1 and Vulkan headers (or ANDROID_NDK_HOME).
No device, network, or game payloads. Deliberate broken variants must fail.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'android/core-link'))
import link_symbols
NATIVE = ROOT/'android/native'
class DispatchSnapshot(unittest.TestCase):
    def test_snapshot_and_negative_controls(self):
        if not sys.platform.startswith('linux'):
            raise RuntimeError('dispatch snapshot test requires Linux libvulkan.so.1; not an Android/device test')
        host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64' if sys.platform.startswith('linux') else None
        if not host:
            raise RuntimeError('compute host test requires Linux or macOS')
        ndk = link_symbols.find_ndk()
        if ndk is None:
            raise RuntimeError('compute host test needs Android NDK 29.0.14206865; no download or skip')
        vulkan = ndk / 'toolchains/llvm/prebuilt' / host / 'sysroot/usr/include/vulkan'
        if not (vulkan / 'vulkan.h').is_file():
            raise RuntimeError('NDK Vulkan headers missing: ' + str(vulkan))
        with tempfile.TemporaryDirectory(prefix='w2-dispatch-') as td:
            out=Path(td);inc=out/'include';(inc/'android').mkdir(parents=True);(inc/'sys').mkdir()
            (inc/'vulkan').symlink_to(vulkan)
            if (vulkan.parent/'vk_video').exists(): (inc/'vk_video').symlink_to(vulkan.parent/'vk_video')
            (inc/'android/log.h').write_text('#pragma once\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\nstatic inline int __android_log_print(int p,const char*t,const char*f,...){(void)p;(void)t;(void)f;return 0;}\n')
            (inc/'sys/system_properties.h').write_text('#pragma once\n#define PROP_VALUE_MAX 92\nstatic inline int __system_property_get(const char*k,char*v){(void)k;*v=0;return 0;}\n')
            source=(NATIVE/'direct/vk_record.c').read_text()
            mutations={
                'positive':source,
                'negative_deferred_ga_identity':source.replace('if (nvn_ga2ca) addr = nvn_ga2ca(addr);', '/* deferred GA conversion removed */'),
                'negative_ssbo_wrong_stage':source.replace('memcpy(ds.ssbo, S->st.ssbo[5], sizeof ds.ssbo);',
                    'memcpy(ds.ssbo, S->st.ssbo[4], sizeof ds.ssbo);'),
                'negative_ssbo_unbind_bit':source.replace('else S->st.ssbo_bound[stage] &= ~((uint64_t)1 << v.index);', 'else (void)0;'),
                'negative_ssbo_last_only':source.replace('S->st.ssbo[stage][v.index].addr = addr;',
                    'memset(S->st.ssbo[stage], 0, sizeof S->st.ssbo[stage]); S->st.ssbo[stage][v.index].addr = addr;'),
                'negative_missing_retirement_callback':source.replace(
                    'e->keep.compute_finish(d, e->keep.compute_owner, REC_SUBMIT_COMPLETED)', '0'),
                'negative_graphics_alias':source.replace('ds.program = S->cs_program;', 'ds.program = S->bound_program;'),
                'negative_stage_packed':source.replace('memcpy(ds.keys[0], S->cs_key, 8);','memcpy(ds.keys[5], S->cs_key, 8);'),
                'negative_ubo_stage':source.replace('ds.ubo_use[5] =', 'ds.ubo_use[0] ='),
                'negative_missing_host_barrier':source.replace(
                    'vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,',
                    'if (0) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,'),
                'negative_host_access':source.replace(
                    '.dstAccessMask = VK_ACCESS_HOST_READ_BIT,\n                };',
                    '.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,\n                };'),
                # P0-0a 变异：去掉生产入口的间接/multi-draw/transform-feedback 闸 → 正例必须红
                # （间接绘制会以补 0 的 count/first 继续走到 builder，变成静默空画）。
                'negative_drop_draw_param_gate':source.replace('if (ind || mdi_cnt || mdi_i || tfc) {', 'if (0) {'),
                # P0-1 变异：不把 base_instance 拷进快照（Vulkan 侧 firstInstance 就回到固定 0）。
                'negative_drop_base_instance':source.replace('ds.base_instance = (uint32_t)d->base_instance;', 'ds.base_instance = 0;'),
                # P0-3 变异：图形掩码退回旧的 5 位截断（高位信息丢失，着色器与 layout 会不一致）。
                'negative_truncate_ubo_mask':source.replace('ds.ubo_use[st] = (uint32_t)(bound << 3);', 'ds.ubo_use[st] = (uint32_t)((bound << 3) & 0x1fu);'),

            }
            for label,code in mutations.items():
                with self.subTest(label=label):
                    if label!='positive': self.assertNotEqual(source,code)
                    src=out/(label+'.c');src.write_text(code);exe=out/label
                    cmd=[os.environ.get('CC','cc'),'-std=c11','-D_POSIX_C_SOURCE=200809L','-O0']
                    cmd += ['-I'+str(p) for p in (inc,NATIVE/'direct',NATIVE/'nvn',ROOT/'graphics/nvn/gfx/nvn',ROOT/'graphics/nvn/gfx/nvn/include')]
                    cmd += [str(ROOT/'android/tests/w2_dispatch_snapshot_test.c'),str(src)]
                    cmd += [str(NATIVE/p) for p in ('direct/bind_defer.c','direct/t2t_plan.c','direct/buffer_texture_plan.c','direct/vk_clear.c','direct/vk_device.c','direct/gs_path.c','nvn/nvn_vk_format.c','nvn/nvn_vk_query.c')]
                    cmd += ['-Wl,-l:libvulkan.so.1','-lm','-o',str(exe)]
                    built=subprocess.run(cmd,text=True,capture_output=True)
                    self.assertEqual(built.returncode,0,built.stderr)
                    run=subprocess.run([str(exe)],text=True,capture_output=True)
                    if label=='positive': self.assertEqual(run.returncode,0,run.stderr)
                    else: self.assertNotEqual(run.returncode,0,'negative control unexpectedly passed')
                    print(label,run.returncode,run.stdout.strip() or run.stderr.strip())
if __name__ == '__main__': unittest.main()
