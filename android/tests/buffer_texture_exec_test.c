// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// Test-only stand-ins around the production B2T/T2B executor (extracted verbatim from
// vk_record.c by buffer_texture_exec_test.py) and the real planner. Not a GPU test:
// the fake vkCmdCopy* moves bytes between staging and a CPU image model by the region it was given.
#include "direct/buffer_texture_plan.h"
#include "include/nvn.h"
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct s2a_vk_device { int unused; };
struct s2a_record_memory {
    void *opaque;
    int (*read)(void *, uint64_t, void *, size_t);
    int (*write)(void *, uint64_t, const void *, size_t);
};
struct rec_keep { VkBuffer buf[1]; VkDeviceMemory mem[1]; int nbuf; };
enum { REC_SUBMIT_INCOMPLETE = 2, REC_SUBMIT_COMPLETED = 1 };

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); return 1; } } while (0)

// Guest memory: 512 bytes; reads/writes outside it fail. Every touched byte is counted.
static uint8_t guest[512], touched[512];
static int fail_read;
static int read_guest(void *o, uint64_t addr, void *dst, size_t n) {
    (void)o; if (fail_read || addr > sizeof guest || n > sizeof guest - addr) return -1;
    memcpy(dst, guest + addr, n); memset(touched + addr, 1, n); return 0;
}
static int write_guest(void *o, uint64_t addr, const void *src, size_t n) {
    (void)o; if (addr > sizeof guest || n > sizeof guest - addr) return -1;
    memcpy(guest + addr, src, n); memset(touched + addr, 1, n); return 0;
}

// Image model: up to 6 layers of a 16x16 level with 4 bytes per texel, one level kept (the plan's mip).
#define IMG_W 16
#define IMG_H 16
#define IMG_L 6
static uint8_t image[IMG_L][IMG_H][IMG_W * 4];
static uint8_t *staging; static size_t staging_size; static int staging_live;
static unsigned texel_bytes = 4, block_w = 1, block_h = 1;
static VkBufferImageCopy last_region; static unsigned copies, barriers_image, barriers_buffer;
static VkImageMemoryBarrier last_before, last_after;

static int create_visible(struct s2a_vk_device *d, VkDeviceSize n, VkBuffer *b, VkDeviceMemory *m, void **p) {
    (void)d; staging = malloc((size_t)n + 64); if (!staging) return -1;   // guard: a wrong executor fails an assertion, not the heap
    staging_size = (size_t)n; staging_live = 1;
    memset(staging, 0xcc, (size_t)n); *p = staging; *b = (VkBuffer)(uintptr_t)1; *m = (VkDeviceMemory)(uintptr_t)1; return 0;
}
static void destroy_visible(struct s2a_vk_device *d, VkBuffer b, VkDeviceMemory m, void *p) {
    (void)d; (void)b; (void)m; free(p); staging = NULL; staging_live = 0;
}
static VkCommandBuffer begin_command(struct s2a_vk_device *d) { (void)d; return (VkCommandBuffer)(uintptr_t)1; }
static int submit(struct s2a_vk_device *d, VkCommandBuffer c, const struct rec_keep *k) { (void)d; (void)c; (void)k; return REC_SUBMIT_COMPLETED; }

// Moves tight staging blocks <-> image by the region (bufferRowLength / imageHeight 0 = tight).
static void move(const VkBufferImageCopy *r, int to_buffer) {
    copies++; last_region = *r;
    uint32_t bx = (r->imageExtent.width + block_w - 1) / block_w, by = (r->imageExtent.height + block_h - 1) / block_h;
    size_t row = (size_t)bx * texel_bytes, slice = row * by;
    for (uint32_t l = 0; l < r->imageSubresource.layerCount; l++)
        for (uint32_t y = 0; y < by; y++) {
            uint8_t *img = &image[r->imageSubresource.baseArrayLayer + l][r->imageOffset.y / block_h + y][(r->imageOffset.x / block_w) * texel_bytes];
            uint8_t *buf = staging + r->bufferOffset + l * slice + y * row;
            if (to_buffer) memcpy(buf, img, row); else memcpy(img, buf, row);
        }
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b,
                                                  uint32_t n, const VkBufferImageCopy *r) {
    (void)c; (void)i; (void)l; (void)b; for (uint32_t k = 0; k < n; k++) move(r + k, 1);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l,
                                                  uint32_t n, const VkBufferImageCopy *r) {
    (void)c; (void)i; (void)l; (void)b; for (uint32_t k = 0; k < n; k++) move(r + k, 0);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags a, VkPipelineStageFlags b,
    VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *bb,
    uint32_t ni, const VkImageMemoryBarrier *ib) {
    (void)c; (void)a; (void)b; (void)f; (void)nm; (void)m; (void)bb;
    barriers_buffer += nb;
    for (uint32_t i = 0; i < ni; i++) { if (copies) last_after = ib[i]; else last_before = ib[i]; barriers_image++; }
}

// Runner extracts these functions verbatim from production vk_record.c.
#include "buffer_texture_exec.inc"

static void reset(void) {
    memset(guest, 0xa5, sizeof guest); memset(touched, 0, sizeof touched); memset(image, 0x11, sizeof image);
    copies = barriers_image = barriers_buffer = 0; fail_read = 0;
    texel_bytes = 4; block_w = block_h = 1;
    memset(&last_region, 0, sizeof last_region); memset(&last_before, 0, sizeof last_before); memset(&last_after, 0, sizeof last_after);
}
static int plan_of(struct s2a_bt_input *in, struct s2a_bt_plan *p) {
    char why[128]; int rc = s2a_buffer_texture_plan(in, p, why, sizeof why);
    if (rc) fprintf(stderr, "planner: %s\n", why);
    return rc;
}
static struct s2a_bt_input cube_input(int direction) {
    // 2x2 RGBA8 cube, face 4 via region z, guest rows 12 bytes apart at address 100 (4 bytes padding).
    struct s2a_bt_input in = {0};
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 1, 6, 1, NVN_TEXTURE_TARGET_CUBEMAP};
    in.view.has = 1; in.region = (struct s2a_bt_region){0, 0, 4, 2, 2, 1};
    in.direction = direction; in.row_stride = 12; in.guest_address = 100;
    in.max_staging_bytes = in.max_guest_span = 4096;
    return in;
}

static int upload(void) {
    struct s2a_vk_device d = {0}; struct s2a_record_memory m = {NULL, read_guest, write_guest};
    struct s2a_bt_input in = cube_input(S2A_BT_TO_TEXTURE); struct s2a_bt_plan p;
    reset();
    for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) guest[100 + y * 12 + x] = (uint8_t)(0x40 + y * 8 + x);
    CHECK(plan_of(&in, &p) == 0);
    CHECK(copy_buffer_texture(&d, (VkImage)(uintptr_t)7, &p, &m) == 0);
    // Exactly the two 8-byte rows were read; the padding between them was not.
    for (int i = 0; i < (int)sizeof guest; i++)
        CHECK(touched[i] == ((i >= 100 && i < 108) || (i >= 112 && i < 120)));
    CHECK(copies == 1 && last_region.imageSubresource.baseArrayLayer == 4 && last_region.imageSubresource.layerCount == 1);
    CHECK(last_region.imageSubresource.mipLevel == 0 && last_region.imageSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(last_region.imageExtent.width == 2 && last_region.imageExtent.height == 2 && last_region.imageExtent.depth == 1);
    CHECK(last_region.bufferRowLength == 0 && last_region.bufferImageHeight == 0 && last_region.bufferOffset == 0);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) CHECK(image[4][y][x] == 0x40 + y * 8 + x);
    CHECK(image[3][0][0] == 0x11 && image[5][0][0] == 0x11);   // other faces untouched
    CHECK(barriers_image == 2 && barriers_buffer == 0);
    CHECK(last_before.image == (VkImage)(uintptr_t)7 && last_before.subresourceRange.baseArrayLayer == 4 &&
          last_before.subresourceRange.layerCount == 1 && last_before.dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT);
    CHECK(last_after.srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT && last_after.subresourceRange.baseArrayLayer == 4);
    CHECK(!staging_live);
    return 0;
}
static int readback(void) {
    struct s2a_vk_device d = {0}; struct s2a_record_memory m = {NULL, read_guest, write_guest};
    struct s2a_bt_input in = cube_input(S2A_BT_TO_BUFFER); struct s2a_bt_plan p;
    reset();
    for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) image[4][y][x] = (uint8_t)(0x70 + y * 8 + x);
    CHECK(plan_of(&in, &p) == 0);
    CHECK(copy_buffer_texture(&d, (VkImage)(uintptr_t)7, &p, &m) == 0);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 8; x++) CHECK(guest[100 + y * 12 + x] == 0x70 + y * 8 + x);
    CHECK(guest[108] == 0xa5 && guest[111] == 0xa5 && guest[120] == 0xa5 && guest[99] == 0xa5);   // padding / tail kept
    CHECK(barriers_image == 2 && barriers_buffer == 1);
    CHECK(last_before.dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT && last_after.srcAccessMask == VK_ACCESS_TRANSFER_READ_BIT);
    CHECK(!staging_live);
    return 0;
}
static int levels(void) {
    // Level 2 of a 16x16 RGBA8 (4x4), tightly packed: one guest read, mipLevel 2 in region and barriers.
    struct s2a_vk_device d = {0}; struct s2a_record_memory m = {NULL, read_guest, write_guest};
    struct s2a_bt_input in = {0}; struct s2a_bt_plan p;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA8, VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 5, 1, 1, NVN_TEXTURE_TARGET_2D};
    in.view = (struct s2a_bt_view){1, 0, 1, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0};
    in.region = (struct s2a_bt_region){0, 0, 0, 4, 4, 1}; in.direction = S2A_BT_TO_TEXTURE;
    in.max_staging_bytes = in.max_guest_span = 4096;
    reset();
    CHECK(plan_of(&in, &p) == 0);
    CHECK(copy_buffer_texture(&d, (VkImage)(uintptr_t)7, &p, &m) == 0);
    CHECK(last_region.imageSubresource.mipLevel == 2 && last_before.subresourceRange.baseMipLevel == 2 &&
          last_after.subresourceRange.baseMipLevel == 2 && last_before.subresourceRange.levelCount == 1);
    for (int i = 0; i < (int)sizeof guest; i++) CHECK(touched[i] == (i < 64));
    return 0;
}
static int compressed(void) {
    // 8x8 BC1 (2x2 blocks of 8 bytes): extent stays in texels, staging holds 32 bytes.
    struct s2a_vk_device d = {0}; struct s2a_record_memory m = {NULL, read_guest, write_guest};
    struct s2a_bt_input in = {0}; struct s2a_bt_plan p;
    in.texture = (struct s2a_bt_texture){NVN_FORMAT_RGBA_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 8, 1, 1, 1, 1, NVN_TEXTURE_TARGET_2D};
    in.region = (struct s2a_bt_region){0, 0, 0, 8, 8, 1}; in.direction = S2A_BT_TO_TEXTURE; in.row_stride = 20;
    in.max_staging_bytes = in.max_guest_span = 4096;
    reset(); texel_bytes = 8; block_w = block_h = 4;
    for (int i = 0; i < 40; i++) guest[i] = (uint8_t)i;
    CHECK(plan_of(&in, &p) == 0);
    CHECK(copy_buffer_texture(&d, (VkImage)(uintptr_t)7, &p, &m) == 0);
    CHECK(staging_size == 32 && last_region.imageExtent.width == 8 && last_region.imageExtent.height == 8);
    for (int i = 0; i < (int)sizeof guest; i++) CHECK(touched[i] == (i < 16 || (i >= 20 && i < 36)));
    CHECK(image[0][0][0] == 0 && image[0][0][15] == 15 && image[0][1][0] == 20 && image[0][1][15] == 35);
    return 0;
}
static int failures(void) {
    struct s2a_vk_device d = {0}; struct s2a_record_memory m = {NULL, read_guest, write_guest};
    struct s2a_bt_input in = cube_input(S2A_BT_TO_TEXTURE); struct s2a_bt_plan p;
    reset(); CHECK(plan_of(&in, &p) == 0);
    fail_read = 1;
    CHECK(copy_buffer_texture(&d, (VkImage)(uintptr_t)7, &p, &m) == -1);
    CHECK(copies == 0 && !staging_live);   // nothing recorded, staging released
    CHECK(copy_buffer_texture(&d, VK_NULL_HANDLE, &p, &m) == -1);
    return 0;
}
int main(int argc, char **argv) {
    const struct { const char *name; int (*test)(void); } groups[] = {
        {"upload", upload}, {"readback", readback}, {"levels", levels}, {"compressed", compressed}, {"failures", failures}
    };
    for (unsigned i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        if (argc > 1 && strcmp(argv[1], groups[i].name)) continue;
        if (groups[i].test()) return 1;
        printf("PASS %s\n", groups[i].name);
    }
    printf("executor: %u assertions; production function with stand-ins, no GPU/device\n", checks);
    return 0;
}
