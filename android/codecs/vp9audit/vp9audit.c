// VP9 representability audit (offline, host): decode an IVF stream with the pinned libvpx, then per shown
// frame repack it as McVp9Backend would — real compressed header (defaults -> supplied prob deltas) — feed
// the rewritten stream to a second libvpx decoder and require byte-identical output pixels. Frames that are
// not repackable (hidden, seg, no-slot, inexpressible probs) end the native segment exactly as
// Vp9NativePath.Fallback does: managed until the next key frame.
// Usage: vp9audit <file.ivf>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"
#include "vpx/internal/vpx_codec_internal.h"
#include "vpx_dsp/bitreader.h"
#include "vp9/common/vp9_onyxc_int.h"
#include "vp9/common/vp9_entropymode.h"
#include "vp9/decoder/vp9_decoder.h"
#include "vp9/decoder/vp9_dsubexp.h"
#include "vp9/vp9_dx_iface.h"
#include "../vpxbench/hdr.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- uncompressed-header bit parser (profile 0): yields the bit length (so tile data can be located),
// first_partition_size and refresh_frame_flags ------------------------------------------

typedef struct {
    const uint8_t *p;
    size_t n, bit;
    int err;
} rb;

static int rb_bit(rb *b)
{
    if (b->bit >> 3 >= b->n) { b->err = 1; return 0; }
    int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static uint32_t rb_lit(rb *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (uint32_t)rb_bit(b);
    return v;
}

static int rb_signed(rb *b, int n)
{
    int v = (int)rb_lit(b, n);
    return rb_bit(b) ? -v : v;
}

typedef struct {
    int err, show_existing, seg_enabled;
    size_t uncompressed_bytes;
    uint32_t first_partition_size;
    uint8_t refresh_flags;
    int profile;
    int ref_idx[3];
} uc_hdr;

static void tile_range(int mi_cols, int *min_log2, int *max_log2)
{
    const int sb64_cols = (mi_cols + 7) >> 3;
    int mn = 0, mx = 1;
    while ((64 << mn) < sb64_cols) mn++;
    while ((sb64_cols >> mx) >= 4) mx++;
    *min_log2 = mn;
    *max_log2 = mx - 1;
}

// known_w: the decoded frame width — needed for the tile-bits span because found_ref frames carry no
// w/h literal (the width comes from a reference buffer, unknown to a pure bitstream parser).
static uc_hdr parse_uc(const uint8_t *buf, size_t sz, int known_w)
{
    uc_hdr h;
    memset(&h, 0, sizeof h);
    rb b = { buf, sz, 0, 0 };
    if (rb_lit(&b, 2) != 2) { h.err = 1; return h; }            // frame_marker
    h.profile = rb_bit(&b) | (rb_bit(&b) << 1);
    if (h.profile == 3) h.profile += rb_bit(&b);
    if (h.profile != 0) { h.err = 1; return h; }   // repack emits profile 0 only; other profiles lay out differently
    if (rb_bit(&b)) { h.show_existing = 1; return h; }          // show_existing_frame
    int key = !rb_bit(&b);                                      // frame_type
    int show_frame = rb_bit(&b);
    int err_res = rb_bit(&b);
    if (key)
    {
        rb_lit(&b, 24);                                         // sync code
        if (h.profile > 0) { rb_lit(&b, 2); }                   // (unused on profile 0)
        else rb_lit(&b, 3);                                     // color_space
        rb_bit(&b);                                             // color_range
        rb_lit(&b, 16); rb_lit(&b, 16);
        if (rb_bit(&b)) rb_lit(&b, 32);                         // render size
        h.refresh_flags = 0xff;
    }
    else
    {
        int intra_only = show_frame ? 0 : rb_bit(&b);
        if (!err_res) rb_lit(&b, 2);                            // reset_frame_context
        if (intra_only)
        {
            rb_lit(&b, 24);                                     // sync code
            h.refresh_flags = (uint8_t)rb_lit(&b, 8);
            rb_lit(&b, 16); rb_lit(&b, 16);
            if (rb_bit(&b)) rb_lit(&b, 32);
        }
        else
        {
            h.refresh_flags = (uint8_t)rb_lit(&b, 8);
            for (int i = 0; i < 3; i++) { h.ref_idx[i] = (int)rb_lit(&b, 3); rb_bit(&b); }
            int found = 0;
            for (int i = 0; i < 3 && !found; i++) found = rb_bit(&b);
            if (!found) { rb_lit(&b, 16); rb_lit(&b, 16); }
            if (rb_bit(&b)) rb_lit(&b, 32);                     // render_and_frame_size_different
            rb_bit(&b);                                         // allow_high_precision_mv
            if (!rb_bit(&b)) rb_lit(&b, 2);                     // interp_filter literal
        }
    }
    if (!err_res) { rb_bit(&b); rb_bit(&b); }                   // refresh_frame_context, parallel mode
    rb_lit(&b, 2);                                              // frame_context_idx
    rb_lit(&b, 6); rb_lit(&b, 3);                               // filter_level, sharpness
    if (rb_bit(&b))                                             // mode_ref_delta_enabled
    {
        if (rb_bit(&b))                                         // mode_ref_delta_update
            for (int i = 0; i < 6; i++) if (rb_bit(&b)) rb_signed(&b, 6);
    }
    rb_lit(&b, 8);                                              // base_q_idx
    for (int i = 0; i < 3; i++) if (rb_bit(&b)) rb_signed(&b, 4); // y/uv dc/ac deltas
    if (rb_bit(&b))                                             // segmentation_enabled
    {
        h.seg_enabled = 1;
        int update_map = rb_bit(&b);
        if (update_map)
        {
            for (int i = 0; i < 7; i++) if (rb_bit(&b)) rb_lit(&b, 8);
            if (rb_bit(&b)) for (int i = 0; i < 3; i++) if (rb_bit(&b)) rb_lit(&b, 8);
        }
        if (rb_bit(&b))                                         // update_data
        {
            rb_bit(&b);                                         // abs_delta
            for (int s = 0; s < 8; s++)
                for (int f = 0; f < 4; f++)
                    if (rb_bit(&b))
                    {
                        static const int bits[4] = { 8, 6, 2, 0 };
                        int v = rb_lit(&b, bits[f]);
                        if (bits[f]) rb_bit(&b);                // sign (except skip which has none)
                        (void)v;
                    }
        }
    }
    int mn, mx;
    tile_range((known_w + 7) >> 3, &mn, &mx);
    for (int k = mn; k < mx && rb_bit(&b); k++) { }             // tile cols run
    if (rb_bit(&b)) rb_bit(&b);                                 // tile rows
    h.first_partition_size = rb_lit(&b, 16);
    h.uncompressed_bytes = (b.bit + 7) >> 3;
    h.err |= b.err;
    return h;
}

// ---- FRAME_CONTEXT -> 3087-byte supplied-probs blob (kf/seg regions filled from defaults) ----

static void fc_to_blob(const FRAME_CONTEXT *fc, uint8_t *blob)
{
    memcpy(blob + 1000, fc->y_mode_prob, 36);
    memcpy(blob + 1036, fc->uv_mode_prob, 90);
    memcpy(blob + 1174, fc->partition_prob, 48);
    memcpy(blob + 1222, fc->coef_probs, 4 * 2 * 216);
    memcpy(blob + 2950, fc->switchable_interp_prob, 8);
    memcpy(blob + 2958, fc->inter_mode_probs, 21);
    memcpy(blob + 2979, fc->intra_inter_prob, 4);
    memcpy(blob + 2983, fc->comp_inter_prob, 5);
    memcpy(blob + 2988, fc->single_ref_prob, 10);
    memcpy(blob + 2998, fc->comp_ref_prob, 5);
    memcpy(blob + 3003, fc->tx_probs.p32x32, 6);
    memcpy(blob + 3009, fc->tx_probs.p16x16, 4);
    memcpy(blob + 3013, fc->tx_probs.p8x8, 2);
    memcpy(blob + 3015, fc->skip_probs, 3);
    memcpy(blob + 3018, fc->nmvc.joints, 3);
    for (int c = 0; c < 2; c++)
    {
        const nmv_component *mc = &fc->nmvc.comps[c];
        blob[3021 + c] = mc->sign;
        memcpy(blob + 3023 + c * 10, mc->classes, 10);
        blob[3043 + c] = mc->class0[0];
        memcpy(blob + 3045 + c * 10, mc->bits, 10);
        memcpy(blob + 3065 + c * 6, mc->class0_fp, 6);
        memcpy(blob + 3077 + c * 3, mc->fp, 3);
        blob[3083 + c] = mc->class0_hp;
        blob[3085 + c] = mc->hp;
    }
}

static int planes_equal(const vpx_image_t *a, const vpx_image_t *b)
{
    if (!a || !b) return 0;
    if (a->d_w != b->d_w || a->d_h != b->d_h || a->fmt != b->fmt) return 0;
    for (int plane = 0; plane < 3; plane++)
    {
        unsigned w, h;
        if (plane == 0) { w = a->d_w; h = a->d_h; }
        else { w = (a->d_w + ((1 << a->x_chroma_shift) - 1)) >> a->x_chroma_shift;
               h = (a->d_h + ((1 << a->y_chroma_shift) - 1)) >> a->y_chroma_shift; }
        for (unsigned y = 0; y < h; y++)
            if (memcmp(a->planes[plane] + y * a->stride[plane], b->planes[plane] + y * b->stride[plane], w))
                return 0;
    }
    return 1;
}

// Pristine spec defaults, byte-identical to Vp9.Decoder.DefaultProbs(): run libvpx's own
// vp9_setup_past_independence on a scratch VP9_COMMON, then convert the resulting FRAME_CONTEXT.
static void build_defaults(uint8_t *defaults)
{
    VP9_COMMON cm0;
    FRAME_CONTEXT fc0;
    memset(&cm0, 0, sizeof cm0);
    memset(&fc0, 0, sizeof fc0);
    cm0.fc = &fc0;
    cm0.frame_contexts = calloc(FRAME_CONTEXTS, sizeof(FRAME_CONTEXT));
    cm0.frame_type = KEY_FRAME;
    vp9_setup_past_independence(&cm0);
    memset(defaults, 0, 3087);
    fc_to_blob(&fc0, defaults);
    free(cm0.frame_contexts);
}

// ---- supplied probs = pre-frame ctx + this frame's compressed-header application ----
// (*cm->fc can't be used post-decode: vp9_adapt_* rewrote it in place. The driver's Entropy blob
// is the post-application table, so apply the ORIGINAL frame's diffs onto its loaded ctx here —
// read_compressed_header's exact field order against the blob offsets.)

static void mv_upd(vpx_reader *r, uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (vpx_read(r, 252)) p[i] = (uint8_t)((vpx_read_literal(r, 7) << 1) | 1);   // MV_UPDATE_PROB
}

static void diff_rng(vpx_reader *r, uint8_t *blob, int off, int n)
{
    for (int i = 0; i < n; i++) vp9_diff_update_prob(r, &blob[off + i]);
}

static int pic_compound_allowed(const s2a_pic *p)
{
    return p->sign_bias[2] != p->sign_bias[1] || p->sign_bias[3] != p->sign_bias[1];
}

static int apply_compressed(uint8_t *blob, const s2a_pic *p, const uint8_t *data, size_t len)
{
    vpx_reader r;
    if (vpx_reader_init(&r, data, len, NULL, NULL)) return -1;
    const int key = (p->flags & S2A_KEY) != 0;
    const int intra_only = !key && (p->flags & S2A_INTRA_ONLY);
    int tx = p->lossless ? 0 : (int)vpx_read_literal(&r, 2);
    if (!p->lossless && tx == 3) tx = vpx_read_bit(&r) ? 4 : 3;
    if (tx != p->tx_mode) return -2;
    if (tx == 4) { diff_rng(&r, blob, S2A_OFF_TX8, 2); diff_rng(&r, blob, S2A_OFF_TX16, 4); diff_rng(&r, blob, S2A_OFF_TX32, 6); }
    for (int t = 0, maxt = tx < 3 ? tx : 3; t <= maxt; t++)
        if (vpx_read_bit(&r))
            for (int i = 0; i < 4; i++)
                for (int b = 0; b < 6; b++)
                    for (int e = 0; e < (b == 0 ? 3 : 6) * 3; e++)
                        vp9_diff_update_prob(&r, &blob[S2A_OFF_COEF + t * 432 + (i * 6 + b) * 18 + e]);
    diff_rng(&r, blob, S2A_OFF_SKIP, 3);
    if (key || intra_only) return vpx_reader_has_error(&r) ? -1 : 0;
    diff_rng(&r, blob, S2A_OFF_INTERMODE, 21);
    if (p->interp == 4) diff_rng(&r, blob, S2A_OFF_SWINTERP, 8);
    diff_rng(&r, blob, S2A_OFF_INTRAINTER, 4);
    int ref = 0;
    if (pic_compound_allowed(p)) ref = vpx_read_bit(&r) ? (vpx_read_bit(&r) ? 2 : 1) : 0;
    if (ref != p->ref_mode) return -3;
    if (ref == 2) diff_rng(&r, blob, S2A_OFF_COMPINTER, 5);
    if (ref != 1) diff_rng(&r, blob, S2A_OFF_SINGLEREF, 10);
    if (ref != 0) diff_rng(&r, blob, S2A_OFF_COMPREF, 5);
    diff_rng(&r, blob, S2A_OFF_YMODE, 36);
    diff_rng(&r, blob, S2A_OFF_PARTITION, 48);
    mv_upd(&r, blob + S2A_OFF_JOINTS, 3);
    for (int c = 0; c < 2; c++)
    {
        mv_upd(&r, blob + S2A_OFF_SIGN + c, 1);
        mv_upd(&r, blob + S2A_OFF_CLASSES + c * 10, 10);
        mv_upd(&r, blob + S2A_OFF_CLASS0 + c, 1);
        mv_upd(&r, blob + S2A_OFF_BITS + c * 10, 10);
    }
    for (int c = 0; c < 2; c++)
    {
        mv_upd(&r, blob + S2A_OFF_CLASS0FP + c * 6, 6);
        mv_upd(&r, blob + S2A_OFF_FP + c * 3, 3);
    }
    if (p->allow_hp)
        for (int c = 0; c < 2; c++)
        {
            mv_upd(&r, blob + S2A_OFF_CLASS0HP + c, 1);
            mv_upd(&r, blob + S2A_OFF_HP + c, 1);
        }
    return vpx_reader_has_error(&r) ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) { fprintf(stderr, "usage: vp9audit <file.ivf> [-guest]\n"); return 2; }
    const int guest = argc == 3 && !strcmp(argv[2], "-guest");
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "DKIF", 4)) { fprintf(stderr, "not ivf\n"); return 1; }

    vpx_codec_ctx_t orig, rw;
    if (vpx_codec_dec_init(&orig, vpx_codec_vp9_dx(), NULL, 0)) { fprintf(stderr, "init failed\n"); return 1; }
    if (vpx_codec_dec_init(&rw, vpx_codec_vp9_dx(), NULL, 0)) { fprintf(stderr, "init2 failed\n"); return 1; }
    struct vpx_codec_alg_priv *alg = (struct vpx_codec_alg_priv *)orig.priv;

    uint8_t *blob = malloc(3087), *defaults = calloc(3087, 1), *au = malloc(1 << 20);
    build_defaults(defaults);
    uint8_t (*ctx_blobs)[3087] = malloc(sizeof(uint8_t[4][3087]));   // post-decode ctx snapshot per idx
    for (int i = 0; i < 4; i++) memcpy(ctx_blobs[i], defaults, 3087);
    int frame = 0, n_frames = 0, n_super = 0, active = 0;
    int slot_ord[8];                    // orig stream's ref-slot map: slot -> producing frame ord
    for (int i = 0; i < 8; i++) slot_ord[i] = -1;
    int *ord_w = malloc(sizeof(int) * 65536), *ord_h = malloc(sizeof(int) * 65536);
    s2a_slots repack;
    s2a_slots_reset(&repack);
    // -guest: model the game's supplied-probs shadow under zero counts. A job the native backend
    // decodes writes no backward-update counts, so the game's next supplied blob stays frozen at
    // the last value real counts produced; a job that went managed resyncs the shadow to its real
    // table (optimistic bound — a managed decode on a desynced base may recover less).
    uint8_t shadow[3087]; memcpy(shadow, defaults, 3087);
    int mc_era = 0;
    int n_match = 0, n_mismatch = 0, n_hidden = 0, n_check = 0, n_hdr = 0, n_slot = 0;
    int n_inactive = 0, n_exist = 0, n_derr = 0, n_profile = 0;

    for (;;)
    {
        uint8_t fh[12];
        if (fread(fh, 1, 12, f) != 12) break;
        uint32_t sz = rd32(fh);
        uint8_t *buf = malloc(sz);
        if (fread(buf, 1, sz, f) != sz) { free(buf); break; }
        frame++;

        // split the superframe index the way vp9_parse_superframe_index does:
        // trailing byte 110xxxxx -> member sizes table + matching front marker byte
        uint32_t msizes[8], moffs[9] = { 0 };
        int nmem = 1;
        msizes[0] = sz;
        if (sz >= 2 && (buf[sz - 1] & 0xe0) == 0xc0)
        {
            int nf = (buf[sz - 1] & 7) + 1, mag = ((buf[sz - 1] >> 3) & 3) + 1;
            size_t isz = 2 + (size_t)mag * nf;
            if (sz >= isz && buf[sz - isz] == buf[sz - 1])
            {
                nmem = nf;
                const uint8_t *x = buf + sz - isz + 1;
                for (int i = 0; i < nf; i++)
                {
                    uint32_t s = 0;
                    for (int j = 0; j < mag; j++) s |= (uint32_t)(*x++) << (8 * j);
                    msizes[i] = s;
                }
                for (int i = 0; i < nf; i++) moffs[i + 1] = moffs[i] + msizes[i];
                n_super++;
            }
        }
        for (int mi = 0; mi < nmem; mi++)
        {
        const uint8_t *mbuf = buf + moffs[mi];
        size_t msz = msizes[mi];
        int ord = ++n_frames;

        vpx_codec_err_t err = vpx_codec_decode(&orig, mbuf, msz, NULL, 0);
        if (err) { fprintf(stderr, "frame %d decode error\n", frame); free(buf); break; }
        VP9_COMMON *cm = &alg->pbi->common;
        uc_hdr uh = parse_uc(mbuf, msz, (int)cm->width);
        int key = cm->frame_type == KEY_FRAME;
        int intra_only = cm->intra_only && !key;
        int show = cm->show_frame;

        if (uh.err || uh.show_existing)
        {
            // unparseable (non-profile-0) or show_existing: can't repack; orig still tracked for ref bookkeeping
            mc_era = 0;
            if (uh.show_existing) { n_exist++; continue; }
            if (key) { active = 1; s2a_slots_reset(&repack); }
            if (active) { printf("f%04d -> hdr(parse/profile)\n", ord); n_profile++; active = 0; s2a_slots_reset(&repack); }
            else n_inactive++;
            continue;
        }

        ord_w[ord & 0xffff] = (int)cm->width;
        ord_h[ord & 0xffff] = (int)cm->height;
        // this frame's loaded ctx (pre-diff): defaults on key/err_res/reset2/reset3, else prior snapshot
        // at the loaded idx — captured BEFORE the snapshot update below
        uint8_t pre_blob[3087];
        if (key || cm->error_resilient_mode || cm->reset_frame_context == 3 || cm->reset_frame_context == 2)
            memcpy(pre_blob, defaults, 3087);
        else
            memcpy(pre_blob, ctx_blobs[cm->frame_context_idx & 3], 3087);
        // ref slots resolve against the PRE-refresh map; refresh_flags then point this frame's output
        // into the slots it claims (keyframes: 0xff -> all eight)
        int ref_ord[3] = { -1, -1, -1 };
        if (!key && !intra_only)
            for (int r = 0; r < 3; r++) ref_ord[r] = slot_ord[uh.ref_idx[r]];
        for (int i = 0; i < 8; i++)
            if (uh.refresh_flags & (1 << i)) slot_ord[i] = ord;
        // post-decode frame_contexts = next frame's pre-state (write-back included)
        for (int i = 0; i < 4; i++) fc_to_blob(&cm->frame_contexts[i], ctx_blobs[i]);

        if (key) { active = 1; s2a_slots_reset(&repack); }
        if (!active) { n_inactive++; mc_era = 0; continue; }

        const char *bucket = NULL;
        if (!show) { bucket = "hidden"; n_hidden++; }
        else if (uh.seg_enabled || cm->seg.enabled) { bucket = "hdr(seg)"; n_hdr++; }
        else if (cm->error_resilient_mode) { bucket = "hdr(err_res)"; n_hdr++; }
        else
        {
            s2a_pic p;
            memset(&p, 0, sizeof p);
            p.flags = (key ? S2A_KEY : 0) | (intra_only ? S2A_INTRA_ONLY : 0) |
                      (cm->error_resilient_mode ? S2A_ERROR_RES : 0);
            for (int i = 0; i < 4; i++) { p.size_w[i] = (uint16_t)cm->width; p.size_h[i] = (uint16_t)cm->height; }
            for (int r = 0; r < 3 && !key && !intra_only; r++)
            {
                int o = ref_ord[r];
                if (o > 0) { p.size_w[r] = (uint16_t)ord_w[o & 0xffff]; p.size_h[r] = (uint16_t)ord_h[o & 0xffff]; }
            }
            for (int i = 1; i < 4; i++) p.sign_bias[i] = (int8_t)cm->ref_frame_sign_bias[i];
            p.filter_level = (uint8_t)cm->lf.filter_level;
            p.sharpness = (uint8_t)cm->lf.sharpness_level;
            p.base_q = (uint8_t)cm->base_qindex;
            // negative delta_q can't be expressed by s2a_pic (rejected downstream like the C# `is < 0` gate)
            p.y_dc = (uint8_t)cm->y_dc_delta_q;
            p.uv_dc = (uint8_t)cm->uv_dc_delta_q;
            p.uv_ac = (uint8_t)cm->uv_ac_delta_q;
            p.lossless = cm->base_qindex == 0 && !cm->y_dc_delta_q && !cm->uv_dc_delta_q && !cm->uv_ac_delta_q;
            p.tx_mode = (uint8_t)cm->tx_mode;
            p.allow_hp = (uint8_t)cm->allow_high_precision_mv;
            // NVDEC pic[121]/s2a_pic.interp carries the bitstream literal (managed Decoder.cs does
            // LiteralToFilter itself), not libvpx's enum — feed the literal like production does.
            static const uint8_t lit_of_enum[5] = { 1, 0, 2, 3, 4 };
            p.interp = lit_of_enum[cm->interp_filter];
            p.ref_mode = (uint8_t)cm->reference_mode;
            p.log2_tile_cols = (uint8_t)cm->log2_tile_cols;
            p.log2_tile_rows = (uint8_t)cm->log2_tile_rows;
            p.seg_enabled = 0;
            p.mode_ref_delta_enabled = (uint8_t)cm->lf.mode_ref_delta_enabled;
            for (int i = 0; i < 4; i++) p.ref_deltas[i] = (int8_t)cm->lf.ref_deltas[i];
            for (int i = 0; i < 2; i++) p.mode_deltas[i] = (int8_t)cm->lf.mode_deltas[i];
            p.surface_params = 8 << 1;   // 8-bit

            s2a_surfaces sf;
            memset(&sf, 0, sizeof sf);
            for (int r = 0; r < 3 && !key && !intra_only; r++)
                sf.luma[r] = sf.chroma[r] = (uint32_t)ref_ord[r];
            sf.luma[3] = sf.chroma[3] = (uint32_t)ord;
            // supplied probs = this frame's loaded ctx + the ORIGINAL compressed header's diffs
            memcpy(blob, pre_blob, 3087);
            const uint8_t *ch = mbuf + uh.uncompressed_bytes;
            int ap = apply_compressed(blob, &p, ch, uh.first_partition_size);
            const uint8_t *supplied = blob;
            if (guest && ap >= 0)
            {
                // 客体零计数模型：客体自己的熵模型照样吃压缩头增量，缺的只是解码计数
                // 的反向适配——MC 作业回来的是全零计数，影子链只靠增量漂移；托管作业
                // 按乐观上界把影子重同步回真值表。
                if (!mc_era) memcpy(shadow, pre_blob, 3087);
                else if (key || cm->error_resilient_mode || cm->reset_frame_context == 3 || cm->reset_frame_context == 2)
                    memcpy(shadow, defaults, 3087);
                apply_compressed(shadow, &p, ch, uh.first_partition_size);
                supplied = shadow;
            }
            const char *chk = ap < 0 ? "ch-apply" : s2a_compressed_check(&p, supplied, defaults);
            if (chk) { bucket = chk; n_check++; }
            else
            {
                const uint8_t *tiles = mbuf + uh.uncompressed_bytes + uh.first_partition_size;
                size_t tiles_len = msz - uh.uncompressed_bytes - uh.first_partition_size;
                size_t len = 0;
                int target = -1;
                const char *be = s2a_build_frame2(&p, &sf, &repack, supplied, defaults, tiles, tiles_len,
                                                  au, 1 << 20, &len, &target);
                if (getenv("AUDIT_DUMP") && ord == (uint32_t)atoi(getenv("AUDIT_DUMP")))
                {
                    if (!be)
                    {
                        FILE *o = fopen("/tmp/au.bin", "wb");
                        fwrite(au, 1, len, o);
                        fclose(o);
                    }
                    FILE *o2 = fopen("/tmp/orig.bin", "wb");
                    fwrite(mbuf, 1, msz, o2);
                    fclose(o2);
                }
                if (be) { bucket = be; n_slot++; }
                else
                {
                    if (vpx_codec_decode(&rw, au, len, NULL, 0)) { bucket = "decode-fail"; n_derr++; }
                    else
                    {
                        vpx_codec_iter_t it1 = NULL, it2 = NULL;
                        const vpx_image_t *ia = vpx_codec_get_frame(&orig, &it1);
                        const vpx_image_t *ib = vpx_codec_get_frame(&rw, &it2);
                        // get_frame yields newest first; the original decoder's latest output is this frame's
                        if (planes_equal(ia, ib))
                        {
                            n_match++;
                            mc_era = 1;
                            s2a_slots_commit(&repack, &p, &sf, target);
                            printf("f%04d %s tx=%d ref=%d prevmv=%d -> Decoded\n", ord, key ? "K" : "I",
                                   cm->tx_mode, cm->reference_mode, cm->use_prev_frame_mvs);
                            continue;
                        }
                        bucket = "PIXEL-MISMATCH";   // wrong pixels poison B's refs -> segment ends like any failure
                        n_mismatch++;
                    }
                }
            }
        }
        // not representable / failed: native segment ends until the next key frame (Vp9NativePath.Fallback)
        printf("f%04d %s tx=%d ref=%d prevmv=%d -> %s\n", ord, key ? "K" : "I",
               cm->tx_mode, cm->reference_mode, cm->use_prev_frame_mvs, bucket);
        active = 0;
        mc_era = 0;
        s2a_slots_reset(&repack);
        }
        free(buf);
    }

    int n = n_frames - n_exist;
    printf("outcome%s: frames=%d (show_existing=%d superframes=%d) decoded=%d mismatch=%d hidden=%d check=%d hdr=%d slot=%d inactive=%d profile=%d decerr=%d\n",
           guest ? " [guest/zero-counts]" : "",
           n_frames, n_exist, n_super, n_match, n_mismatch, n_hidden, n_check, n_hdr, n_slot, n_inactive, n_profile, n_derr);
    if (n) printf("decoded share: %d/%d = %.1f%%\n", n_match, n, 100.0 * n_match / n);
    return 0;
}
