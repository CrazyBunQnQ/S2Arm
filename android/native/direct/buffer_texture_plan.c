// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
#include "buffer_texture_plan.h"
#include "nvn_vk_format.h"
#include "nvn_fmt_info.h"
#include "include/nvn.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int fail(int code, char *why, size_t n, const char *message) {
    if (why && n) snprintf(why, n, "%s", message);
    return code;
}
static int boolean(int v) { return v == 0 || v == 1; }
static struct s2a_bt_dependency dependency(int direction, int after, uint32_t aspect,
                                           uint32_t mip, uint32_t base_layer, uint32_t layer_count) {
    struct s2a_bt_dependency d = {0};
    uint32_t transfer = direction == S2A_BT_TO_BUFFER ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    d.src_stage = after ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    d.dst_stage = after ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    d.src_access = after ? transfer : VK_ACCESS_MEMORY_WRITE_BIT;
    d.dst_access = after ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : transfer;
    d.old_layout = d.new_layout = VK_IMAGE_LAYOUT_GENERAL;
    d.aspect = aspect;
    d.mip = mip; d.base_layer = base_layer; d.layer_count = layer_count;
    return d;
}
static int layered(int32_t target) {
    return target == NVN_TEXTURE_TARGET_2D_ARRAY || target == NVN_TEXTURE_TARGET_CUBEMAP ||
           target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
}
static uint32_t mip_size(uint32_t size, uint32_t level) {
    return level >= 32 || !(size >> level) ? 1 : size >> level;
}

int s2a_buffer_texture_plan(const struct s2a_bt_input *in, struct s2a_bt_plan *out, char *why, size_t n) {
    if (out) memset(out, 0, sizeof *out);
    if (why && n) why[0] = 0;
    if (!in || !out) return fail(S2A_BT_INVALID, why, n, "missing planner input/output");
    if (in->direction != S2A_BT_TO_TEXTURE && in->direction != S2A_BT_TO_BUFFER)
        return fail(S2A_BT_INVALID, why, n, "invalid copy direction");
    const struct s2a_bt_texture *t = &in->texture;
    const struct s2a_bt_view *v = &in->view;
    const struct s2a_bt_region *r = &in->region;
    if (!t->width || !t->height || !t->depth || t->width > INT32_MAX || t->height > INT32_MAX ||
        t->depth > INT32_MAX || !t->levels || !t->layers)
        return fail(S2A_BT_INVALID, why, n, "missing or invalid image dimensions/subresources");
    int is_3d = t->target == NVN_TEXTURE_TARGET_3D;
    if ((t->target != NVN_TEXTURE_TARGET_2D && !is_3d && !layered(t->target)) || t->samples > 1)
        return fail(S2A_BT_UNSUPPORTED, why, n, "only single-sample 2D, 2D array, cube and 3D images are covered");
    if ((is_3d && t->layers != 1) || (!is_3d && t->depth != 1) ||
        (t->target == NVN_TEXTURE_TARGET_2D && t->layers != 1) ||
        (t->target == NVN_TEXTURE_TARGET_CUBEMAP && t->layers != 6) ||
        (t->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && t->layers % 6))
        return fail(S2A_BT_INVALID, why, n, "image depth/layers do not match its target");
    // LINEAR_FILTER (0x1) / ENGINE_2D (0x2) choose how a scaled copy is filtered and which engine runs it: a
    // buffer <-> texture copy is 1:1, so they change nothing (Metal's copy_b2t / copy_t2b never read flags).
    // Mirror bits would change the bytes and stay refused.
    if (in->flags & ~0x3u) {
        if (why && n) snprintf(why, n, "copy flags 0x%x are not covered", (unsigned)in->flags);
        return S2A_BT_UNSUPPORTED;
    }
    nvn_fmt_info fmt;
    if (!nvn_fmt_info_of(t->nvn_format, &fmt) || fmt.stencil || fmt.expand || !fmt.bw || !fmt.bh)
        return fail(S2A_BT_UNSUPPORTED, why, n, "unknown/stencil/converted format is not covered");
    VkFormat expected = nvn_vk_format(t->nvn_format);
    if (expected == VK_FORMAT_UNDEFINED || (uint32_t)expected != t->vk_format)
        return fail(S2A_BT_UNSUPPORTED, why, n, "guest format does not match actual image storage");
    if (!boolean(v->has)) return fail(S2A_BT_INVALID, why, n, "invalid view presence");
    uint32_t level = 0, layer0 = 0;
    if (v->has) {
        if (!boolean(v->set_format) || !boolean(v->set_levels) || !boolean(v->set_layers) ||
            !boolean(v->set_target) || !boolean(v->set_swizzle) || !boolean(v->set_dsmode))
            return fail(S2A_BT_INVALID, why, n, "invalid view field presence");
        if ((v->set_format && v->format != t->nvn_format) ||
            (v->set_target && v->target != t->target) || v->set_dsmode)
            return fail(S2A_BT_UNSUPPORTED, why, n, "non-default view interpretation is not covered");
        // A count of 0 is "to the last level / layer" (games set base 5, count 0 on a 6-level image);
        // the copy itself only uses the base, as Metal's copy_b2t does.
        if (v->set_levels) {
            if (v->base_level < 0 || v->nlevels < 0 || (uint32_t)v->base_level >= t->levels ||
                (uint32_t)v->nlevels > t->levels - (uint32_t)v->base_level) {
                if (why && n) snprintf(why, n, "view mip range %d+%d is outside image (%u levels)",
                                       v->base_level, v->nlevels, t->levels);
                return S2A_BT_INVALID;
            }
            level = (uint32_t)v->base_level;
        }
        if (v->set_layers) {
            if (v->min_layer < 0 || v->nlayers < 0 || (uint32_t)v->min_layer >= t->layers ||
                (uint32_t)v->nlayers > t->layers - (uint32_t)v->min_layer) {
                if (why && n) snprintf(why, n, "view layer range %d+%d is outside image (%u layers)",
                                       v->min_layer, v->nlayers, t->layers);
                return S2A_BT_INVALID;
            }
            layer0 = (uint32_t)v->min_layer;
        }
    }
    if (r->x < 0 || r->y < 0 || r->z < 0 || r->width <= 0 || r->height <= 0 || r->depth < 0)
        return fail(S2A_BT_INVALID, why, n, "invalid region");
    uint32_t lw = mip_size(t->width, level), lh = mip_size(t->height, level);
    uint32_t slices = r->depth ? (uint32_t)r->depth : 1;
    uint32_t room = is_3d ? mip_size(t->depth, level) : t->layers - layer0;
    if ((uint32_t)r->x > lw || (uint32_t)r->width > lw - (uint32_t)r->x ||
        (uint32_t)r->y > lh || (uint32_t)r->height > lh - (uint32_t)r->y ||
        (uint32_t)r->z > room || slices > room - (uint32_t)r->z) {
        if (why && n) snprintf(why, n, "region %d,%d,%d %dx%dx%u is outside level %u (%ux%u, %u slices)",
                               r->x, r->y, r->z, r->width, r->height, slices, level, lw, lh, room);
        return S2A_BT_INVALID;
    }
    // Vulkan copies whole blocks: offsets on block edges, sizes whole blocks or up to the level edge.
    if (r->x % fmt.bw || r->y % fmt.bh || ((uint32_t)r->width % fmt.bw && (uint32_t)(r->x + r->width) != lw) ||
        ((uint32_t)r->height % fmt.bh && (uint32_t)(r->y + r->height) != lh))
        return fail(S2A_BT_INVALID, why, n, "region is not aligned to the format's blocks");
    // A single row fits uint64_t. Check the row-pitch product before forming
    // the full span/staging size (INT32_MAX squared times 16 can overflow).
    uint64_t bpp = fmt.bytes;
    uint64_t row_bytes = (((uint64_t)(uint32_t)r->width + fmt.bw - 1) / fmt.bw) * bpp;
    uint64_t pitch = in->row_stride ? in->row_stride : row_bytes;
    if (pitch < row_bytes) return fail(S2A_BT_INVALID, why, n, "row stride is shorter than a row");
    uint64_t rows = ((uint64_t)(uint32_t)r->height + fmt.bh - 1) / fmt.bh;
    if (pitch > UINT64_MAX / rows) return fail(S2A_BT_INVALID, why, n, "image pitch overflows");
    uint64_t slice_span = (rows - 1) * pitch + row_bytes;
    uint64_t image_stride = in->image_stride ? in->image_stride : pitch * rows;
    if (image_stride < slice_span) return fail(S2A_BT_INVALID, why, n, "image stride is shorter than touched bytes");
    if (image_stride > (UINT64_MAX - slice_span) / slices)
        return fail(S2A_BT_INVALID, why, n, "image stride overflows");
    uint64_t span = (uint64_t)(slices - 1) * image_stride + slice_span;
    if (in->guest_address > UINT64_MAX - span)
        return fail(S2A_BT_INVALID, why, n, "guest address extent overflows");
    if (row_bytes * rows > UINT64_MAX / slices) return fail(S2A_BT_INVALID, why, n, "staging size overflows");
    uint64_t staging = row_bytes * rows * slices;
    if (!in->max_staging_bytes || !in->max_guest_span)
        return fail(S2A_BT_INVALID, why, n, "missing explicit planner budgets");
    if (staging > in->max_staging_bytes || staging > SIZE_MAX || span > in->max_guest_span)
        return fail(S2A_BT_LIMIT, why, n, "copy exceeds staging or guest span budget");
    struct s2a_bt_plan p = {0};
    p.direction = in->direction;
    p.x = (uint32_t)r->x; p.y = (uint32_t)r->y;
    p.width = (uint32_t)r->width; p.height = (uint32_t)r->height;
    p.bytes_per_texel = (uint32_t)bpp;
    p.row_bytes = row_bytes; p.guest_row_pitch = pitch; p.image_stride = image_stride;
    p.guest_address = in->guest_address; p.guest_span = span; p.staging_bytes = staging;
    p.aspect = fmt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    p.mip = level;
    p.base_layer = is_3d ? 0 : layer0 + (uint32_t)r->z;
    p.layer_count = is_3d ? 1 : slices;
    p.z = is_3d ? (uint32_t)r->z : 0;
    p.depth = is_3d ? slices : 1;
    p.block_width = fmt.bw; p.block_height = fmt.bh;
    p.rows = (uint32_t)rows; p.slices = slices;
    p.image_before = dependency(in->direction, 0, p.aspect, p.mip, p.base_layer, p.layer_count);
    p.image_after = dependency(in->direction, 1, p.aspect, p.mip, p.base_layer, p.layer_count);
    *out = p;
    return S2A_BT_OK;
}

int s2a_buffer_texture_row(const struct s2a_bt_plan *p, uint32_t row, struct s2a_bt_span *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!p || !out || !p->rows || row / p->rows >= p->slices) return S2A_BT_INVALID;
    out->guest_address = p->guest_address + (uint64_t)(row / p->rows) * p->image_stride +
                         (uint64_t)(row % p->rows) * p->guest_row_pitch;
    out->staging_offset = (uint64_t)row * p->row_bytes;
    out->bytes = p->row_bytes;
    return S2A_BT_OK;
}
