// NVDEC VP9 job → frame for libvpx's S2Arm hook (vpx/s2a_vp9_nvdec.h): C mirror of the header writer and slot map in
// docs/设计_原生VP9/design_phase2_B.md §2-§3. Used by vpxbench --jobs (PR 1 speed and parity check) and, in later PRs,
// as the second implementation the C# writer is tested against.
#ifndef S2A_VPXBENCH_HDR_H
#define S2A_VPXBENCH_HDR_H

#include <stddef.h>
#include <stdint.h>

#define S2A_NVDEC_PICINFO_SIZE 256
#define S2A_NVDEC_ENTROPY_SIZE 3744
#define S2A_NVDEC_COUNTS_SIZE 13264   // BackwardUpdates: inter-mode counts as [7][3][2]

// Byte offsets of each compressed-header-updateable section inside the 3087-byte Ryujinx
// Vp9EntropyProbs blob. Shared by the writer (hdr.c) and audit-side decoders (vp9audit).
enum {
  S2A_OFF_YMODE = 1000, S2A_OFF_UV = 1036, S2A_OFF_PARTITION = 1174, S2A_OFF_COEF = 1222,
  S2A_OFF_SWINTERP = 2950, S2A_OFF_INTERMODE = 2958, S2A_OFF_INTRAINTER = 2979,
  S2A_OFF_COMPINTER = 2983, S2A_OFF_SINGLEREF = 2988, S2A_OFF_COMPREF = 2998,
  S2A_OFF_TX32 = 3003, S2A_OFF_TX16 = 3009, S2A_OFF_TX8 = 3013, S2A_OFF_SKIP = 3015,
  S2A_OFF_JOINTS = 3018, S2A_OFF_SIGN = 3021, S2A_OFF_CLASSES = 3023, S2A_OFF_CLASS0 = 3043,
  S2A_OFF_BITS = 3045, S2A_OFF_CLASS0FP = 3065, S2A_OFF_FP = 3077, S2A_OFF_CLASS0HP = 3083,
  S2A_OFF_HP = 3085
};

// The PictureInfo fields the header needs (Ryujinx.Graphics.Nvdec.Types.Vp9.PictureInfo).
typedef struct {
    uint32_t bitstream_size;
    uint16_t size_w[4], size_h[4];   // Last, Golden, Alt, Current
    uint32_t flags;                  // FrameFlags
    int8_t sign_bias[4];
    uint8_t filter_level, sharpness, base_q, y_dc, uv_ac, uv_dc, lossless, tx_mode, allow_hp, interp, ref_mode;
    int8_t comp_fixed_ref, comp_var_ref[2];
    uint8_t log2_tile_cols, log2_tile_rows;
    uint8_t seg_enabled;
    uint8_t mode_ref_delta_enabled;
    int8_t ref_deltas[4], mode_deltas[2];
    uint32_t surface_params;
} s2a_pic;

enum { S2A_KEY = 1, S2A_LAST_KEY = 2, S2A_SIZE_CHANGED = 4, S2A_ERROR_RES = 8, S2A_LAST_SHOW = 16, S2A_INTRA_ONLY = 32 };

typedef struct {
    uint32_t luma, chroma;
    int w, h, valid;
    uint64_t last_use;
} s2a_slot;

typedef struct {
    s2a_slot s[8];
    uint64_t now;
} s2a_slots;

// Guest surface offsets for one job: Last, Golden, Alt, output (SetPictureLuma/ChromaOffset[0..3]).
typedef struct {
    uint32_t luma[4], chroma[4];
} s2a_surfaces;

void s2a_pic_parse(const uint8_t *picinfo, s2a_pic *p);
// NVDEC EntropyProbs (3744 bytes) → Ryujinx Vp9EntropyProbs layout (3087 bytes), as EntropyProbs.Convert.
void s2a_entropy_convert(const uint8_t *nvdec, uint8_t *out);
// Vp9BackwardUpdates (13208 bytes) → NVDEC BackwardUpdates layout (S2A_NVDEC_COUNTS_SIZE bytes), as the BackwardUpdates constructor. Returns bytes written.
size_t s2a_counts_to_guest(const uint8_t *counts, uint8_t *out);

// Header checks (§2) and slot resolution (§3). On success writes header + 1 byte + tiles into dst and returns NULL;
// otherwise returns the reason the job must go to C#. *target receives the slot the output refreshes.
const char *s2a_build_frame(const s2a_pic *p, const s2a_surfaces *sf, s2a_slots *slots, const uint8_t *tiles,
                            size_t tiles_size, uint8_t *dst, size_t cap, size_t *len, int *target);
// Same, but the compressed header is real: defaults -> supplied prob deltas (spec §6.3). probs/defaults are the
// 3087-byte Ryujinx-layout blobs. The caller should run s2a_compressed_check first for sections without update slots.
const char *s2a_build_frame2(const s2a_pic *p, const s2a_surfaces *sf, s2a_slots *slots,
                             const uint8_t *probs, const uint8_t *defaults, const uint8_t *tiles,
                             size_t tiles_size, uint8_t *dst, size_t cap, size_t *len, int *target);
// Compressed-header expressibility: NULL when every load-bearing prob section can be carried (see
// Vp9CompressedHeader.Check in the C# mirror for the rules).
const char *s2a_compressed_check(const s2a_pic *p, const uint8_t *probs, const uint8_t *defaults);
// Writes just the compressed header; returns the length or -1 on buffer overflow.
int s2a_compressed_write(const s2a_pic *p, const uint8_t *probs, const uint8_t *defaults,
                         uint8_t *dst, size_t cap);
// After a successful native decode: record the output in the slot map.
void s2a_slots_commit(s2a_slots *slots, const s2a_pic *p, const s2a_surfaces *sf, int target);
void s2a_slots_reset(s2a_slots *slots);

// C#'s UsePrevInFindMvRefs from the flags.
int s2a_use_prev_mvs(const s2a_pic *p);

#endif
