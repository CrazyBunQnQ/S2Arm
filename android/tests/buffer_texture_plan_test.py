#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Pure copy-plan contract and four compiled functional mutation controls."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
DIRECT = NATIVE / 'direct'
TEST = Path(__file__).with_suffix('.c')


def main():
    source = (DIRECT / 'buffer_texture_plan.c').read_text()
    mutations = (
        ('ignore-offset', 'p.x = (uint32_t)r->x; p.y = (uint32_t)r->y;', 'p.x = p.y = 0;', 'grid'),
        ('fixed-four-bytes', 'uint64_t bpp = fmt.bytes;', 'uint64_t bpp = 4;', 'formats'),
        ('overwrite-padding', 'out->bytes = p->row_bytes;', 'out->bytes = p->image_stride;', 'grid'),
        ('omit-image-before', 'p.image_before = dependency(in->direction, 0, p.aspect, p.mip, p.base_layer, p.layer_count);',
         'p.image_before = dependency(in->direction, 0, p.aspect, p.mip, p.base_layer, p.layer_count);\n    memset(&p.image_before, 0, sizeof p.image_before);', 'grid'),
        ('omit-image-after', 'p.image_after = dependency(in->direction, 1, p.aspect, p.mip, p.base_layer, p.layer_count);',
         'p.image_after = dependency(in->direction, 1, p.aspect, p.mip, p.base_layer, p.layer_count);\n    memset(&p.image_after, 0, sizeof p.image_after);', 'grid'),
        ('ignore-view-level', 'level = (uint32_t)v->base_level;', 'level = 0;', 'mips'),
        ('ignore-region-layer', 'p.base_layer = is_3d ? 0 : layer0 + (uint32_t)r->z;', 'p.base_layer = is_3d ? 0 : layer0;', 'layers'),
        ('texel-rows-for-blocks', 'uint64_t rows = ((uint64_t)(uint32_t)r->height + fmt.bh - 1) / fmt.bh;',
         'uint64_t rows = (uint32_t)r->height;', 'blocks'),
        ('depth-as-color', 'p.aspect = fmt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;',
         'p.aspect = VK_IMAGE_ASPECT_COLOR_BIT;', 'depth'),
    )
    with tempfile.TemporaryDirectory(prefix='s2a-bt-plan-') as temp:
        build = Path(temp)
        includes = (NATIVE, DIRECT, NATIVE / 'nvn', TEST.parent / 'vkhost',
                    ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/third_party/vulkan-headers/include')

        def run(name, text, group=None):
            implementation = build / 'buffer_texture_plan.c'
            implementation.write_text(text)
            exe = build / name
            cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror']
            cmd += shlex.split(os.environ.get('CFLAGS', '')) + ['-I' + str(p) for p in includes]
            subprocess.run(cmd + [str(TEST), str(implementation), str(NATIVE / 'nvn/nvn_vk_format.c'),
                                 '-o', str(exe)], check=True, timeout=60)
            result = subprocess.run([str(exe)] + ([group] if group else []), capture_output=True, text=True, timeout=10)
            print(name + ':\n' + result.stdout + result.stderr, end='', flush=True)
            return result

        result = run('production', source)
        if result.returncode or result.stdout.count('PASS ') != 10:
            raise RuntimeError('production contract failed')
        for name, old, new, group in mutations:
            if source.count(old) != 1:
                raise RuntimeError('mutation anchor changed: ' + name)
            result = run(name, source.replace(old, new), group)
            if result.returncode != 1 or 'FAIL ' + group + ':' not in result.stderr:
                raise RuntimeError(name + ': expected compiled functional assertion failure')
    print('PASS: 10 host groups and 9 compiled functional controls (8 fault classes); GPU/device not tested here')


if __name__ == '__main__':
    if len(sys.argv) != 1:
        raise SystemExit("usage: buffer_texture_plan_test.py")
    main()
