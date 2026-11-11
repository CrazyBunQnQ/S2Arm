// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Pure plan for already decoded NVN B2T/T2B records. No handles, allocation,
// guest access or Vulkan calls. vk_record.c (OP_COPY_B2T / OP_COPY_T2B) executes it.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum s2a_bt_direction { S2A_BT_TO_TEXTURE = 0, S2A_BT_TO_BUFFER = 1 };
enum s2a_bt_result { S2A_BT_OK = 0, S2A_BT_INVALID = -1, S2A_BT_UNSUPPORTED = -2, S2A_BT_LIMIT = -3 };
struct s2a_bt_texture {
    uint32_t nvn_format, vk_format; // actual guest format and resolved image format, not a guessed override
    uint32_t width, height, depth, levels, layers;
    uint32_t samples; // NVN convention: 0 or 1 means single-sample, not VkSampleCount=0
    int32_t target;
};
struct s2a_bt_view {
    int has, set_format, set_levels, set_layers, set_target, set_swizzle, set_dsmode;
    uint32_t format;
    int32_t base_level, nlevels, min_layer, nlayers, target;
};
struct s2a_bt_region { int32_t x, y, z, width, height, depth; };
struct s2a_bt_input {
    struct s2a_bt_texture texture;
    struct s2a_bt_view view;
    struct s2a_bt_region region;
    int direction;
    uint32_t flags, row_stride, image_stride;
    uint64_t guest_address;
    // Explicit caller budgets. Zero is invalid; this planner never chooses an
    // unbounded staging allocation. Guest span is an extent, not a bulk read.
    uint64_t max_staging_bytes, max_guest_span;
};
struct s2a_bt_dependency {
    uint32_t src_stage, dst_stage, src_access, dst_access;
    uint32_t old_layout, new_layout, aspect, mip, base_layer, layer_count;
};
struct s2a_bt_plan {
    int direction;
    uint32_t x, y, width, height, bytes_per_texel;   // texels; bytes_per_texel = bytes per block
    uint64_t row_bytes, guest_row_pitch, image_stride, guest_address, guest_span, staging_bytes;
    // Each block row packs into tight staging, slice after slice. Neither
    // direction reads/writes guest row padding or image tail. Vulkan
    // bufferOffset is zero, not guest_address; bufferRowLength/imageHeight are
    // zero (tight): one VkBufferImageCopy covers every slice.
    struct s2a_bt_dependency image_before, image_after;
    uint32_t aspect, mip, base_layer, layer_count;   // image subresource (3D: layer 0, count 1)
    uint32_t z, depth;                               // 3D: imageOffset.z / extent depth; else 0 / 1
    uint32_t block_width, block_height;              // texels per block (1 x 1 uncompressed)
    uint32_t rows, slices;                           // block rows per slice; slices (layers or 3D depth)
};
struct s2a_bt_span { uint64_t guest_address, staging_offset, bytes; };

// Single-sample 2D / 2D array / cube / cube array / 3D, color or depth-only,
// uncompressed or block-compressed, identical runtime/storage format, flags=0.
// Mip and first layer as Metal's copy_b2t: view base_level / min_layer
// (set_levels / set_layers), region z = layer offset (3D: depth offset), one
// slice per layer at image_stride. Depth=0 follows NVN/Metal's single-slice
// convention. Swizzle does not change copied bytes; format / target / depth-
// stencil-mode reinterpretations are not covered. Unsupported is not fake
// success. Failure clears output; inputs stay unchanged. why is optional.
int s2a_buffer_texture_plan(const struct s2a_bt_input *input, struct s2a_bt_plan *out,
                           char *why, size_t why_size);
// Plan must be an unmodified successful output from the function above.
// row counts block rows across slices: 0 .. rows * slices - 1.
int s2a_buffer_texture_row(const struct s2a_bt_plan *plan, uint32_t row, struct s2a_bt_span *out);

// Executor obligations (NOT proved by a plan): pin resources and verify
// live image facts/capabilities and current GENERAL layout; handle queue
// ownership before using these same-owner GENERAL-to-GENERAL dependencies
// (they do not transition from UNDEFINED/attachment layouts); resolve GPU-written
// source buffers coherently;
// copy only these spans; flush/invalidate staging as needed; emit both image
// dependencies and the host-buffer dependencies; submit in owner order; only
// unpack/free after proven completion, retain resources if completion is unknown.
