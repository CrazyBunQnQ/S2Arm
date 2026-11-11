#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Compile the production alias scan and serial helpers, without a Vulkan device.

Checks candidate selection and serial helpers only, not Vulkan allocation,
GPU content, or OOM. Production drop/retire/pin helpers are compiled too;
release is a counting stub and the optional serial index is disabled.
VULKAN_HEADERS may point to genuine Vulkan headers (e.g. NDK sysroot/usr/include)
when the optional vendored header checkout is unavailable.
--source-file compiles an unchanged baseline for a withdrawal control.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-file', type=Path)
a = p.parse_args()
src = (a.source_file or ROOT / 'android/native/nvn/nvn_vk_texcache.c').read_text()

def function(signature):
    start = src.index(signature + ' {')
    end = src.index('{', start) + 1
    depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]

start = src.index('    // 活对象主表按 serial 定位')
end = src.index('    if (o && (o->serial', start)
scan = src[start:end]
# Exclude the preceding serial lookup: tests start with a fresh serial (o=NULL).
scan = scan[scan.index('    if (!o) {'):]
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nvn_vk_texcache.h"
#define ANDROID_LOG_INFO 4
#define __android_log_print(...) ((void)0)
'''
code += function('static int tex_has_serial(const struct nvn_vk_tex *t, uint64_t serial)')
code += '\n' + function('static void tex_add_serial(struct nvn_vk_tex *t, uint64_t serial)')
code += '\n' + function('static int tex_remove_serial(struct nvn_vk_tex *t, uint64_t serial)')
code += r'''
// Test seams: no GPU destruction; count releases and invalidate synthetic handles.
// Serial index stays disabled; production find_serial takes its full-scan fallback.
static int release_calls;
static void release(struct s2a_vk_device *d, struct nvn_vk_tex *t) {
    (void)d; release_calls++; t->valid=0; t->image=VK_NULL_HANDLE;
    t->view=VK_NULL_HANDLE; t->bytes=0;
    for (int i=0; i<t->views_cap; i++) t->views[i].valid=0;
}
static void tex_sidx_put(struct nvn_vk_texcache *c, uint64_t serial, int idx) {
    (void)c; (void)serial; (void)idx;
}
'''
for signature in [
    'static int tex_has_pinned_views(const struct nvn_vk_tex *t)',
    'static void retired_release_if_idle(struct nvn_vk_tex *t)',
    'static void tex_tombstones_add(struct nvn_tex_tombstones *tt, struct nvn_tex_tombstone e)',
    'static void slot_retire(struct nvn_vk_texcache *c, struct nvn_vk_tex *t)',
    'static unsigned tex_sidx_hash(uint64_t x)',
    'static struct nvn_vk_tex *tex_find_serial(struct nvn_vk_texcache *c, uint64_t serial, int *slot_out)',
    'void nvn_vk_texcache_drop_tex(struct nvn_vk_texcache *c, const tex_t *t)',
    'void nvn_vk_texcache_pin(struct nvn_vk_tex *t)',
    'void nvn_vk_texcache_unpin(struct nvn_vk_tex *t)',
    'int nvn_vk_texcache_pin_view(struct nvn_vk_tex *t, VkImageView view)',
    'void nvn_vk_texcache_unpin_view(struct nvn_vk_tex *t, VkImageView view)',
]:
    code += '\n' + function(signature)
code += r'''
static struct nvn_vk_tex *select_alias(struct nvn_vk_texcache *c, tex_t *t) {
    uint32_t layers=1, levels=1, depth=1, target=1;
    struct nvn_vk_tex *o = NULL;
'''+scan+r'''
    return o;
}
static int failures;
#define CHECK(name, cond) do { if (!(cond)) { puts("FAIL " name); failures++; } else puts("PASS " name); } while (0)
int main(void) {
    uint8_t backing[16];
    struct nvn_vk_tex x={0}, y={0}, z={0};
    x.valid=1; x.key=(uintptr_t)backing; x.serial=1; x.width=8; x.height=8;
    x.layers=x.levels=x.depth=x.nvn_target=1; x.nvn_format=0x25;
    y=x; y.serial=2; y.width=y.height=16;
    z=y; z.serial=4;
    struct nvn_vk_tex *slots[]={&x,&y,&z};
    struct nvn_vk_texcache c={0}; c.slots=slots; c.nslots=2;
    tex_t t={0}; t.mem=backing; t.serial=3; t.d.width=t.d.height=16; t.d.format=0x25;
    struct nvn_vk_tex *got=select_alias(&c,&t);
    CHECK("A/B/B finds second compatible candidate", got==&y);
    CHECK("new serial belongs to B", tex_has_serial(&y,3));
    CHECK("incompatible A unchanged", x.nref==0 && x.width==8);
    c.nslots=1; t.serial=5;
    CHECK("incompatible-only rejects sharing", select_alias(&c,&t)==NULL);
    c.nslots=3; y.retired=1; t.serial=6;
    CHECK("retired candidate skipped", select_alias(&c,&t)==&z);
    y.retired=0; y.valid=0; t.serial=7;
    CHECK("invalid candidate skipped", select_alias(&c,&t)==&z);
    y.valid=1; y.key++; z.key++; t.serial=8;
    CHECK("different backing rejects sharing", select_alias(&c,&t)==NULL);
    y.key=x.key; y.width=y.height=8; t.d.width=t.d.height=8; t.serial=9;
    CHECK("first compatible still wins", select_alias(&c,&t)==&x);
    struct nvn_vk_tex shared={0}; shared.serial=20; tex_add_serial(&shared,21);
    CHECK("remove alias leaves primary", tex_remove_serial(&shared,21)==0 && shared.serial==20);
    CHECK("remove final primary permits retirement", tex_remove_serial(&shared,20)==1);
    shared.serial=20; shared.nref=0; tex_add_serial(&shared,21);
    CHECK("remove primary promotes alias", tex_remove_serial(&shared,20)==0 && shared.serial==21);
    CHECK("repeat removed primary preserves alias", tex_remove_serial(&shared,20)==0 && shared.serial==21);
    CHECK("remove promoted alias permits retirement", tex_remove_serial(&shared,21)==1);
    for (int reverse=0; reverse<2; reverse++) {
        struct nvn_vk_tex a={0}, b={0};
        struct nvn_vk_tex_view v={0};
        a.valid=b.valid=1; a.serial=1; b.serial=2;
        b.bytes=4096; b.image=(VkImage)(uintptr_t)100;
        v.valid=1; v.view=(VkImageView)(uintptr_t)101;
        b.views=&v; b.views_cap=1;
        struct nvn_vk_tex *pair[]={&a,&b};
        struct nvn_vk_texcache life={0}; life.slots=pair; life.nslots=2;
        a.owner=b.owner=&life; tex_add_serial(&b,3);
        nvn_vk_texcache_pin(&b);
        CHECK("pin view", nvn_vk_texcache_pin_view(&b,v.view)==0);
        tex_t first={0}, last={0}; first.serial=reverse?3:2; last.serial=reverse?2:3;
        int before=release_calls;
        nvn_vk_texcache_drop_tex(&life,&first);
        CHECK("first Finalize preserves shared image", b.valid && !b.retired && release_calls==before);
        nvn_vk_texcache_drop_tex(&life,&last);
        CHECK("last Finalize retires pinned image", b.valid && b.retired && life.retired_n==1 && life.retired_bytes==4096);
        nvn_vk_texcache_drop_tex(&life,&first); nvn_vk_texcache_drop_tex(&life,&last);
        CHECK("repeat Finalize preserves unrelated A", a.valid && a.serial==1 && release_calls==before && life.retired_n==1);
        VkImageView view=v.view;
        if (reverse) nvn_vk_texcache_unpin_view(&b,view); else nvn_vk_texcache_unpin(&b);
        CHECK("one pin class remains no release", b.valid && release_calls==before);
        if (reverse) nvn_vk_texcache_unpin(&b); else nvn_vk_texcache_unpin_view(&b,view);
        CHECK("last pin releases exactly once", !b.valid && release_calls==before+1 && life.retired_n==0 && life.retired_bytes==0);
        nvn_vk_texcache_unpin(&b); nvn_vk_texcache_unpin_view(&b,view);
        CHECK("repeated unpin no double release", release_calls==before+1 && a.valid);
    }
    printf("alias scan/lifecycle: 27 cases, %d failures (no Vulkan device)\n", failures);
    return failures ? 1 : 0;
}
'''
incs = ['android/native/nvn','android/native/direct','graphics/nvn/third_party/vulkan-headers/include','graphics/nvn/gfx/nvn','graphics/nvn/gfx/nvn/include','graphics/nvn/gfx/tex']
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'test.c'; exe=Path(tmp)/'test'; cfile.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror', *[f'-I{ROOT/d}' for d in incs],*(['-idirafter',os.environ['VULKAN_HEADERS']] if os.environ.get('VULKAN_HEADERS') else []),str(cfile),'-o',str(exe)],check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
