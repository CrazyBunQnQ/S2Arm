// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Independent byte-grid oracle and dependency model. No Vulkan execution.
#include "direct/buffer_texture_plan.h"
#include "include/nvn.h"
#include <vulkan/vulkan.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); return 1; } } while (0)
static struct s2a_bt_input input(void) {
    struct s2a_bt_input in = {0};
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_R16, VK_FORMAT_R16_UNORM, 8, 6, 1, 1, 1, 1, NVN_TEXTURE_TARGET_2D};
    in.region = (struct s2a_bt_region){2, 1, 0, 3, 2, 1};
    in.guest_address = 7; in.row_stride = 10; in.image_stride = 32;
    in.max_staging_bytes = 1024 * 1024; in.max_guest_span = 1024 * 1024;
    return in;
}
static int plan(struct s2a_bt_input *in, struct s2a_bt_plan *p) {
    struct s2a_bt_input old = *in;
    char why[128]; int rc = s2a_buffer_texture_plan(in, p, why, sizeof why);
    if (rc) fprintf(stderr, "planner: %s\n", why);
    if (memcmp(&old, in, sizeof old)) return -10;
    return rc;
}
static int rejects(struct s2a_bt_input in, int expected) {
    struct s2a_bt_plan p, zero = {0}; char why[128] = "old";
    memset(&p, 0xa5, sizeof p);
    CHECK(s2a_buffer_texture_plan(&in, &p, why, sizeof why) == expected);
    CHECK(!memcmp(&p, &zero, sizeof p)); CHECK(why[0]);
    return 0;
}
static int dependency_valid(const struct s2a_bt_dependency *d, int direction, int after) {
    unsigned transfer = direction == S2A_BT_TO_BUFFER ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    return d->old_layout == VK_IMAGE_LAYOUT_GENERAL && d->new_layout == VK_IMAGE_LAYOUT_GENERAL &&
        d->aspect == VK_IMAGE_ASPECT_COLOR_BIT && d->mip == 0 && d->base_layer == 0 && d->layer_count == 1 &&
        d->src_stage == (after ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) &&
        d->dst_stage == (after ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT) &&
        d->src_access == (after ? transfer : VK_ACCESS_MEMORY_WRITE_BIT) &&
        d->dst_access == (after ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : transfer);
}

// A shadow visibility model, not a substitute for Vulkan validation. Prior image
// writes become visible to this transfer only on image_before; this transfer's
// writes become visible to the next consumer only on image_after. Removing a
// required dependency changes bytes observed by the test, not only a counter.
static int run_grid(const struct s2a_bt_plan *p, unsigned bpp, uint8_t *guest,
                    uint8_t *actual, uint8_t *observed) {
    uint8_t visible[8 * 6 * 16] = {0}, staging[8 * 6 * 16] = {0};
    if (p->staging_bytes > sizeof staging || p->width > 8 || p->height > 6 ||
        p->x > 8 - p->width || p->y > 6 - p->height || p->bytes_per_texel != bpp) return -1;
    if (dependency_valid(&p->image_before, p->direction, 0)) memcpy(visible, actual, 8 * 6 * bpp);
    for (uint32_t row = 0; row < p->height; row++) {
        struct s2a_bt_span span;
        if (s2a_buffer_texture_row(p, row, &span) || span.guest_address > 128 || span.bytes > 128 - span.guest_address ||
            span.staging_offset > sizeof staging || span.bytes > sizeof staging - span.staging_offset) return -1;
        size_t image_offset = ((p->y + row) * 8 + p->x) * bpp;
        if (p->direction == S2A_BT_TO_TEXTURE) {
            memcpy(staging + span.staging_offset, guest + span.guest_address, (size_t)span.bytes);
            memcpy(actual + image_offset, staging + row * p->row_bytes, (size_t)p->width * bpp);
        } else {
            memcpy(staging + row * p->row_bytes, visible + image_offset, (size_t)p->width * bpp);
            memcpy(guest + span.guest_address, staging + span.staging_offset, (size_t)span.bytes);
        }
    }
    if (dependency_valid(&p->image_after, p->direction, 1)) memcpy(visible, actual, 8 * 6 * bpp);
    memcpy(observed, visible, 8 * 6 * bpp);
    return 0;
}
static int grid(void) {
    struct s2a_bt_input in = input(); struct s2a_bt_plan p;
    uint8_t guest[128], actual[96], observed[96], expected[96], expected_guest[128];
    // Oracle written from literal geometry/format, never planner-derived sizes.
    memset(guest, 0xa5, sizeof guest); memset(actual, 0x51, sizeof actual);
    memcpy(expected, actual, sizeof actual);
    for (unsigned row = 0; row < 2; row++) for (unsigned col = 0; col < 6; col++) {
        uint8_t value = (uint8_t)(0x20 + row * 6 + col);
        guest[7 + row * 10 + col] = value;
        expected[((1 + row) * 8 + 2) * 2 + col] = value;
    }
    memcpy(expected_guest, guest, sizeof guest);
    CHECK(plan(&in, &p) == 0); CHECK(run_grid(&p, 2, guest, actual, observed) == 0);
    CHECK(!memcmp(observed, expected, sizeof expected)); CHECK(!memcmp(guest, expected_guest, sizeof guest));
    in.direction = S2A_BT_TO_BUFFER; memset(guest, 0xa5, sizeof guest); memset(expected_guest, 0xa5, sizeof expected_guest);
    for (unsigned i = 0; i < sizeof actual; i++) actual[i] = (uint8_t)(i + 1);
    for (unsigned row = 0; row < 2; row++) for (unsigned col = 0; col < 6; col++)
        expected_guest[7 + row * 10 + col] = (uint8_t)(((1 + row) * 8 + 2) * 2 + col + 1);
    CHECK(plan(&in, &p) == 0); CHECK(run_grid(&p, 2, guest, actual, observed) == 0);
    CHECK(!memcmp(guest, expected_guest, sizeof guest)); CHECK(!memcmp(actual, observed, sizeof actual));
    return 0;
}
static int formats(void) {
    // Explicit source-independent format oracle, including 1/2/4/8/16 bytes.
    const struct { unsigned nvn, vk, bytes; } cases[] = {
        {NVN_FORMAT_R8, VK_FORMAT_R8_UNORM, 1}, {NVN_FORMAT_RG8, VK_FORMAT_R8G8_UNORM, 2},
        {NVN_FORMAT_R16, VK_FORMAT_R16_UNORM, 2}, {NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 4},
        {NVN_FORMAT_RGBA16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
        {NVN_FORMAT_RGBA32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {NVN_FORMAT_RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4}, {NVN_FORMAT_BGRA8, VK_FORMAT_B8G8R8A8_UNORM, 4}
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct s2a_bt_input in = input(); struct s2a_bt_plan p; struct s2a_bt_span span;
        in.texture.nvn_format = cases[i].nvn; in.texture.vk_format = cases[i].vk;
        in.row_stride = in.image_stride = 0;
        CHECK(plan(&in, &p) == 0); CHECK(p.bytes_per_texel == cases[i].bytes);
        CHECK(p.row_bytes == 3 * cases[i].bytes); CHECK(p.staging_bytes == 6 * cases[i].bytes);
        CHECK(p.guest_span == 6 * cases[i].bytes);
        CHECK(s2a_buffer_texture_row(&p, 1, &span) == 0);
        CHECK(span.guest_address == 7 + 3 * cases[i].bytes && span.bytes == 3 * cases[i].bytes);
    }
    return 0;
}
static int spans(void) {
    struct s2a_bt_input in = input(); struct s2a_bt_plan p; struct s2a_bt_span s, zero = {0};
    CHECK(plan(&in, &p) == 0);
    CHECK(p.x == 2 && p.y == 1 && p.width == 3 && p.height == 2);
    CHECK(p.row_bytes == 6 && p.guest_span == 16 && p.staging_bytes == 12 && p.image_stride == 32);
    CHECK(s2a_buffer_texture_row(&p, 0, &s) == 0 && s.guest_address == 7 && s.bytes == 6 && s.staging_offset == 0);
    CHECK(s2a_buffer_texture_row(&p, 1, &s) == 0 && s.guest_address == 17 && s.bytes == 6 && s.staging_offset == 6);
    memset(&s, 0xa5, sizeof s); CHECK(s2a_buffer_texture_row(&p, 2, &s) == S2A_BT_INVALID);
    CHECK(!memcmp(&s, &zero, sizeof s)); CHECK(s2a_buffer_texture_row(NULL, 0, &s) == S2A_BT_INVALID);
    CHECK(s2a_buffer_texture_row(&p, 0, NULL) == S2A_BT_INVALID);
    in.row_stride = 7; in.image_stride = 13; // odd guest pitch is packed, not passed to Vulkan
    CHECK(plan(&in, &p) == 0); CHECK(p.guest_row_pitch == 7 && p.guest_span == 13 && p.staging_bytes == 12);
    in.region = (struct s2a_bt_region){7, 5, 0, 1, 1, 0}; in.row_stride = 9; in.image_stride = 2;
    CHECK(plan(&in, &p) == 0); CHECK(p.guest_span == 2 && p.staging_bytes == 2);
    return 0;
}
static int views(void) {
    struct s2a_bt_input in = input(); struct s2a_bt_plan p, before;
    CHECK(plan(&in, &before) == 0); in.view.has = 1;
    CHECK(plan(&in, &p) == 0 && !memcmp(&p, &before, sizeof p));
    in.texture.levels = 4;
    in.view = (struct s2a_bt_view){1, 1, 1, 1, 1, 0, 0, NVN_FORMAT_R16, 0, 4, 0, 1, NVN_TEXTURE_TARGET_2D};
    CHECK(plan(&in, &p) == 0); in.view.format = NVN_FORMAT_R16UI; CHECK(rejects(in, S2A_BT_UNSUPPORTED) == 0);
    in = input(); in.view.has = 1; in.view.set_levels = 1; in.view.nlevels = 0;   // count 0 = to the last level
    CHECK(plan(&in, &p) == 0 && !memcmp(&p, &before, sizeof p));
    in.view.nlevels = -1; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    in.view.nlevels = 1; in.view.base_level = 1; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // one-level image
    in = input(); in.view.has = 1; in.view.set_layers = 1; in.view.min_layer = 1; in.view.nlayers = 1;
    CHECK(rejects(in, S2A_BT_INVALID) == 0);   // one-layer image
    in = input(); in.view.has = 1; in.view.set_target = 1; in.view.target = NVN_TEXTURE_TARGET_2D_ARRAY;
    CHECK(rejects(in, S2A_BT_UNSUPPORTED) == 0);
    in = input(); in.view.has = 1; in.view.set_swizzle = 1;   // swizzle changes sampling, not copied bytes
    CHECK(plan(&in, &p) == 0 && !memcmp(&p, &before, sizeof p));
    in.view.set_swizzle = 0; in.view.set_dsmode = 1; CHECK(rejects(in, S2A_BT_UNSUPPORTED) == 0);
    in.view.set_dsmode = 2; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    in = input(); in.view.has = 2; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    in = input(); memset(&in.view, 0xff, sizeof in.view); in.view.has = 0;
    CHECK(plan(&in, &p) == 0); // absent fields are not interpreted
    return 0;
}
static int errors(void) {
    struct s2a_bt_input in = input(); struct s2a_bt_plan p;
#define BAD(field, value, code) do { in = input(); in.field = value; CHECK(rejects(in, code) == 0); } while (0)
    BAD(direction, 9, S2A_BT_INVALID); BAD(flags, 4, S2A_BT_UNSUPPORTED); BAD(flags, UINT32_MAX, S2A_BT_UNSUPPORTED);   // mirror bits change the bytes
    {   // LINEAR_FILTER / ENGINE_2D do not change a 1:1 buffer <-> texture copy: same plan as flags = 0
        struct s2a_bt_plan none; in = input(); CHECK(plan(&in, &none) == 0);
        for (uint32_t f = 1; f <= 3; f++) { in = input(); in.flags = f; CHECK(plan(&in, &p) == 0 && !memcmp(&p, &none, sizeof p)); }
    }
    BAD(texture.width, 0, S2A_BT_INVALID); BAD(texture.width, UINT32_MAX, S2A_BT_INVALID);
    BAD(texture.levels, 0, S2A_BT_INVALID); BAD(texture.layers, 2, S2A_BT_INVALID);
    BAD(texture.depth, 2, S2A_BT_INVALID); BAD(texture.depth, 0, S2A_BT_INVALID);
    BAD(texture.samples, 4, S2A_BT_UNSUPPORTED);
    BAD(texture.target, NVN_TEXTURE_TARGET_1D, S2A_BT_UNSUPPORTED);
    BAD(texture.target, NVN_TEXTURE_TARGET_BUFFER, S2A_BT_UNSUPPORTED);
    BAD(texture.target, NVN_TEXTURE_TARGET_CUBEMAP, S2A_BT_INVALID);   // a cube has six layers
    BAD(texture.nvn_format, NVN_FORMAT_DEPTH24_STENCIL8, S2A_BT_UNSUPPORTED);
    BAD(texture.nvn_format, NVN_FORMAT_RGB8, S2A_BT_UNSUPPORTED);
    BAD(texture.nvn_format, NVN_FORMAT_RGBA_DXT1, S2A_BT_UNSUPPORTED);   // vk_format still R16
    BAD(texture.nvn_format, 0xff, S2A_BT_UNSUPPORTED);
    BAD(texture.vk_format, VK_FORMAT_R8_UNORM, S2A_BT_UNSUPPORTED);
    BAD(region.x, -1, S2A_BT_INVALID); BAD(region.y, INT32_MAX, S2A_BT_INVALID);
    BAD(region.width, 0, S2A_BT_INVALID); BAD(region.height, -1, S2A_BT_INVALID);
    BAD(region.width, INT32_MAX, S2A_BT_INVALID); BAD(region.depth, 2, S2A_BT_INVALID);
    BAD(region.depth, -1, S2A_BT_INVALID); BAD(region.z, 1, S2A_BT_INVALID); BAD(region.z, -1, S2A_BT_INVALID);
    BAD(row_stride, 5, S2A_BT_INVALID);
    BAD(image_stride, 15, S2A_BT_INVALID); BAD(guest_address, UINT64_MAX - 15, S2A_BT_INVALID);
    BAD(max_staging_bytes, 0, S2A_BT_INVALID); BAD(max_guest_span, 0, S2A_BT_INVALID);
    BAD(max_staging_bytes, 11, S2A_BT_LIMIT); BAD(max_guest_span, 15, S2A_BT_LIMIT);
#undef BAD
    in = input(); in.guest_address = UINT64_MAX - 16; CHECK(plan(&in, &p) == 0);
    in = input(); in.texture.samples = 0; CHECK(plan(&in, &p) == 0);
    in = input(); in.max_staging_bytes = 12; in.max_guest_span = 16; CHECK(plan(&in, &p) == 0);
    in = input(); in.texture.width = in.texture.height = INT32_MAX;
    in.region = (struct s2a_bt_region){0, 0, 0, INT32_MAX, INT32_MAX, 1};
    in.texture.nvn_format = NVN_FORMAT_RGBA32F; in.texture.vk_format = VK_FORMAT_R32G32B32A32_SFLOAT;
    in.row_stride = in.image_stride = 0; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    memset(&p, 0xa5, sizeof p); CHECK(s2a_buffer_texture_plan(NULL, &p, NULL, 0) == S2A_BT_INVALID);
    struct s2a_bt_plan zero = {0}; CHECK(!memcmp(&p, &zero, sizeof p));
    CHECK(s2a_buffer_texture_plan(&in, NULL, NULL, 0) == S2A_BT_INVALID);
    in = input(); CHECK(s2a_buffer_texture_plan(&in, &p, NULL, 0) == 0);
    return 0;
}
// Subresources. Every expectation below is written from literal geometry.
static int mips(void) {
    // 32x32 RGBA8, 6 levels; the game uploads level 3 (4x4) through a view with base_level 3.
    struct s2a_bt_input in = input(); struct s2a_bt_plan p; struct s2a_bt_span s;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 6, 1, 1, NVN_TEXTURE_TARGET_2D};
    in.view = (struct s2a_bt_view){1, 0, 1, 0, 0, 0, 0, 0, 3, 1, 0, 0, 0};
    in.region = (struct s2a_bt_region){0, 0, 0, 4, 4, 1}; in.row_stride = in.image_stride = 0;
    CHECK(plan(&in, &p) == 0); CHECK(p.mip == 3 && p.base_layer == 0 && p.layer_count == 1);
    CHECK(p.row_bytes == 16 && p.rows == 4 && p.slices == 1 && p.staging_bytes == 64 && p.guest_span == 64);
    CHECK(p.image_before.mip == 3 && p.image_after.mip == 3);
    CHECK(s2a_buffer_texture_row(&p, 3, &s) == 0 && s.guest_address == 7 + 48 && s.staging_offset == 48);
    in.view.base_level = 5; in.region.width = in.region.height = 1;   // level 5 is 1x1
    CHECK(plan(&in, &p) == 0 && p.mip == 5 && p.staging_bytes == 4);
    in.view.nlevels = 0; CHECK(plan(&in, &p) == 0 && p.mip == 5);     // as recorded by the game: base 5, count 0
    in.view.nlevels = 1;
    in.region.width = 2; CHECK(rejects(in, S2A_BT_INVALID) == 0);    // wider than level 5
    in.view.base_level = 6; in.region.width = 1; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    in.view.base_level = 2; in.view.nlevels = 5; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // 2 + 5 > 6
    return 0;
}
static int layers(void) {
    // A 1x1 RGBA8 cube: one face per copy, the face in region z (Metal: slice = min_layer + z).
    struct s2a_bt_input in = input(); struct s2a_bt_plan p; struct s2a_bt_span s;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 6, 1, NVN_TEXTURE_TARGET_CUBEMAP};
    in.view.has = 1; in.region = (struct s2a_bt_region){0, 0, 5, 1, 1, 1}; in.row_stride = in.image_stride = 0;
    CHECK(plan(&in, &p) == 0); CHECK(p.base_layer == 5 && p.layer_count == 1 && p.z == 0 && p.depth == 1);
    CHECK(p.image_before.base_layer == 5 && p.image_before.layer_count == 1);
    in.region.z = 6; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    // All six faces at once from a 2D array view starting at layer 2 of a 3-cube array: layers 2..7.
    in.texture.layers = 18; in.texture.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    in.view = (struct s2a_bt_view){1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 6, 0};
    in.region = (struct s2a_bt_region){0, 0, 0, 1, 1, 6}; in.image_stride = 32;
    CHECK(plan(&in, &p) == 0); CHECK(p.base_layer == 2 && p.layer_count == 6 && p.slices == 6);
    CHECK(p.staging_bytes == 24 && p.guest_span == 5 * 32 + 4);
    CHECK(s2a_buffer_texture_row(&p, 4, &s) == 0 && s.guest_address == 7 + 4 * 32 && s.staging_offset == 16 && s.bytes == 4);
    CHECK(s2a_buffer_texture_row(&p, 6, &s) == S2A_BT_INVALID);
    in.region.z = 11; in.region.depth = 6; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // 2 + 11 + 6 > 18
    in.region.z = 0; in.texture.layers = 7; CHECK(rejects(in, S2A_BT_INVALID) == 0);  // not whole cubes
    return 0;
}
static int volume(void) {
    // 4x4x8 R16 3D, level 1 = 2x2x4: two depth slices at z 1, one VkBufferImageCopy with depth 2.
    struct s2a_bt_input in = input(); struct s2a_bt_plan p;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_R16, VK_FORMAT_R16_UNORM, 4, 4, 8, 2, 1, 1, NVN_TEXTURE_TARGET_3D};
    in.view = (struct s2a_bt_view){1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0};
    in.region = (struct s2a_bt_region){0, 0, 1, 2, 2, 2}; in.row_stride = in.image_stride = 0;
    CHECK(plan(&in, &p) == 0); CHECK(p.mip == 1 && p.base_layer == 0 && p.layer_count == 1 && p.z == 1 && p.depth == 2);
    CHECK(p.row_bytes == 4 && p.rows == 2 && p.image_stride == 8 && p.staging_bytes == 16 && p.guest_span == 16);
    in.region.z = 3; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // 3 + 2 > 4 slices at level 1
    in.texture.layers = 2; in.region.z = 0; CHECK(rejects(in, S2A_BT_INVALID) == 0);
    return 0;
}
static int blocks(void) {
    // 10x6 BC1 (8 bytes per 4x4 block): 3x2 blocks, the right/bottom blocks partial.
    struct s2a_bt_input in = input(); struct s2a_bt_plan p; struct s2a_bt_span s;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 10, 6, 1, 1, 1, 1, NVN_TEXTURE_TARGET_2D};
    in.region = (struct s2a_bt_region){0, 0, 0, 10, 6, 1}; in.row_stride = in.image_stride = 0;
    CHECK(plan(&in, &p) == 0); CHECK(p.block_width == 4 && p.block_height == 4 && p.bytes_per_texel == 8);
    CHECK(p.row_bytes == 24 && p.rows == 2 && p.staging_bytes == 48 && p.guest_span == 48);
    CHECK(p.aspect == VK_IMAGE_ASPECT_COLOR_BIT);
    in.region = (struct s2a_bt_region){4, 4, 0, 6, 2, 1}; in.row_stride = 40;   // the last block column/row, up to the edge
    CHECK(plan(&in, &p) == 0 && p.row_bytes == 16 && p.rows == 1 && p.guest_span == 16);
    CHECK(s2a_buffer_texture_row(&p, 0, &s) == 0 && s.guest_address == 7 && s.bytes == 16);
    in.region = (struct s2a_bt_region){2, 0, 0, 4, 4, 1}; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // x off the block grid
    in.region = (struct s2a_bt_region){0, 0, 0, 6, 4, 1}; CHECK(rejects(in, S2A_BT_INVALID) == 0);   // partial block not at the edge
    return 0;
}
static int depth(void) {
    // D16 uploads go to the depth aspect; depth-stencil formats stay unsupported.
    struct s2a_bt_input in = input(); struct s2a_bt_plan p;
    in.texture.nvn_format = NVN_FORMAT_DEPTH16; in.texture.vk_format = VK_FORMAT_D16_UNORM;
    CHECK(plan(&in, &p) == 0); CHECK(p.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && p.bytes_per_texel == 2);
    CHECK(p.image_before.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && p.image_after.aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
    in.texture.vk_format = VK_FORMAT_D32_SFLOAT; CHECK(rejects(in, S2A_BT_UNSUPPORTED) == 0);   // substituted storage
    return 0;
}
int main(int argc, char **argv) {
    const struct { const char *name; int (*test)(void); } groups[] = {
        {"grid", grid}, {"formats", formats}, {"spans", spans}, {"views", views}, {"errors", errors},
        {"mips", mips}, {"layers", layers}, {"volume", volume}, {"blocks", blocks}, {"depth", depth}
    };
    for (unsigned i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        if (argc > 1 && strcmp(argv[1], groups[i].name)) continue;
        if (groups[i].test()) return 1;
        printf("PASS %s\n", groups[i].name);
    }
    printf("planner: %u assertions; independent byte-grid/dependency model, no GPU/device\n", checks);
    return 0;
}
