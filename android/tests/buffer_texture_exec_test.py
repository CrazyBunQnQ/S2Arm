#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""B2T/T2B executor contract: the production function from vk_record.c with stand-ins, plus
compiled functional mutation controls. No GPU or device: real-device evidence is in the PR."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
TEST = Path(__file__).with_suffix('.c')
GROUPS = 5


def function(text, marker):
    if text.count(marker) != 1:
        raise RuntimeError('production marker changed: ' + marker)
    start = text.index(marker)
    level = 0
    for i in range(text.index('{', start), len(text)):
        if text[i] == '{':
            level += 1
        elif text[i] == '}':
            level -= 1
            if not level:
                return text[start:i + 1] + '\n'
    raise RuntimeError('unclosed production function')


def extract(source):
    return (function(source, 'static VkImageMemoryBarrier bt_image_barrier(') +
            function(source, 'static int copy_buffer_texture('))


def main():
    source = (NATIVE / 'direct/vk_record.c').read_text()
    executor = extract(source)
    mutations = (
        ('read-image-stride', 'if (s2a_buffer_texture_row(p, row, &span) ||\n                    mem->read(mem->opaque, span.guest_address, (uint8_t *)mapped + span.staging_offset,\n                              (size_t)span.bytes) != 0) goto fail_staging;',
         'if (s2a_buffer_texture_row(p, row, &span) ||\n                    mem->read(mem->opaque, span.guest_address, (uint8_t *)mapped + span.staging_offset,\n                              (size_t)p->guest_row_pitch) != 0) goto fail_staging;', 'upload'),
        ('layer-zero', 'region.imageSubresource = (VkImageSubresourceLayers){p->aspect, p->mip, p->base_layer, p->layer_count};',
         'region.imageSubresource = (VkImageSubresourceLayers){p->aspect, p->mip, 0, p->layer_count};', 'upload'),
        ('mip-zero', 'region.imageSubresource = (VkImageSubresourceLayers){p->aspect, p->mip, p->base_layer, p->layer_count};',
         'region.imageSubresource = (VkImageSubresourceLayers){p->aspect, 0, p->base_layer, p->layer_count};', 'levels'),
        ('omit-before', 'vkCmdPipelineBarrier(cmd, p->image_before.src_stage, p->image_before.dst_stage, 0, 0, NULL, 0, NULL, 1, &before);',
         '(void)before;', 'upload'),
        ('write-padding', 'mem->write(mem->opaque, span.guest_address, (const uint8_t *)mapped + span.staging_offset,\n                               (size_t)span.bytes)',
         'mem->write(mem->opaque, span.guest_address, (const uint8_t *)mapped + span.staging_offset,\n                               (size_t)p->guest_row_pitch)', 'readback'),
        ('leak-on-read-failure', 'fail_staging:\n    destroy_visible(d, buffer, memory, mapped);\n    return -1;',
         'fail_staging:\n    return -1;', 'failures'),
    )
    with tempfile.TemporaryDirectory(prefix='s2a-bt-exec-') as temp:
        build = Path(temp)
        includes = (NATIVE, NATIVE / 'direct', NATIVE / 'nvn', build, ROOT / 'android/tests/vkhost',
                    ROOT / 'graphics/nvn/gfx/nvn', ROOT / 'graphics/nvn/third_party/vulkan-headers/include')

        def run(name, text, group=None):
            (build / 'buffer_texture_exec.inc').write_text(text)
            exe = build / name
            cmd = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror']
            cmd += shlex.split(os.environ.get('CFLAGS', '')) + ['-I' + str(p) for p in includes]
            subprocess.run(cmd + [str(TEST), str(NATIVE / 'direct/buffer_texture_plan.c'),
                                 str(NATIVE / 'nvn/nvn_vk_format.c'), '-o', str(exe)], check=True, timeout=60)
            result = subprocess.run([str(exe)] + ([group] if group else []), capture_output=True, text=True, timeout=10)
            print(name + ':\n' + result.stdout + result.stderr, end='', flush=True)
            return result

        result = run('production', executor)
        if result.returncode or result.stdout.count('PASS ') != GROUPS:
            raise RuntimeError('production executor contract failed')
        for name, old, new, group in mutations:
            if executor.count(old) != 1:
                raise RuntimeError('mutation anchor changed: ' + name)
            result = run(name, executor.replace(old, new), group)
            if result.returncode != 1 or 'FAIL ' + group + ':' not in result.stderr:
                raise RuntimeError(name + ': expected compiled functional assertion failure')
    print(f'PASS: {GROUPS} executor groups and {len(mutations)} compiled functional controls; GPU/device not tested here')


if __name__ == '__main__':
    main()
