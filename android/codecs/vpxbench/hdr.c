// See hdr.h. Field order and checks follow docs/设计_原生VP9/design_phase2_B.md §2 (header) and §3 (slot map).
// Compressed-header 段的写出顺序与概率布局出处：libvpx 1.17.0 (pinned, BSD-3-Clause)
// vp9/decoder/vp9_decodeframe.c `read_compressed_header`、vp9/common/vp9_entropymode.h——与
// compat/tree/src/Ryujinx.Graphics.Nvdec/Vp9CompressedHeader.cs 同款镜像，hdrcmp 金样测字节级一致。
#include "hdr.h"

#include <assert.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void s2a_pic_parse(const uint8_t *b, s2a_pic *p)
{
    memset(p, 0, sizeof *p);
    p->bitstream_size = rd32(b + 48);
    for (int i = 0; i < 4; i++)
    {
        p->size_w[i] = rd16(b + 72 + 8 * i);
        p->size_h[i] = rd16(b + 74 + 8 * i);
    }
    p->flags = rd32(b + 104);
    memcpy(p->sign_bias, b + 108, 4);
    p->filter_level = b[112];
    p->sharpness = b[113];
    p->base_q = b[114];
    p->y_dc = b[115];
    p->uv_ac = b[116];
    p->uv_dc = b[117];
    p->lossless = b[118];
    p->tx_mode = b[119];
    p->allow_hp = b[120];
    p->interp = b[121];
    p->ref_mode = b[122];
    p->comp_fixed_ref = (int8_t)b[123];
    p->comp_var_ref[0] = (int8_t)b[124];
    p->comp_var_ref[1] = (int8_t)b[125];
    p->log2_tile_cols = b[126];
    p->log2_tile_rows = b[127];
    p->seg_enabled = b[128];
    p->mode_ref_delta_enabled = b[228];
    memcpy(p->ref_deltas, b + 229, 4);
    memcpy(p->mode_deltas, b + 233, 2);
    p->surface_params = rd32(b + 240);
}

int s2a_use_prev_mvs(const s2a_pic *p)
{
    uint32_t f = p->flags;
    return !(f & S2A_KEY) && !(f & S2A_ERROR_RES) && !(f & S2A_SIZE_CHANGED) && !(f & S2A_INTRA_ONLY) &&
           (f & S2A_LAST_SHOW) && !(f & S2A_LAST_KEY);
}

// ---- probability and count layouts (Ryujinx structs, sequential, no padding) ----

typedef struct {
    uint8_t kf_y_e0_7[10][10][8], kf_y_e8[10][10], pad384[3], seg_tree[7], seg_pred[3], pad391[15];
    uint8_t kf_uv_e0_7[10][8], kf_uv_e8[10], pad3fa[6], inter_mode[7][4], intra_inter[4], uv_e0_7[10][8];
    uint8_t tx8[2][1], tx16[2][2], tx32[2][3], y_e8[4], y_e0_7[4][8], kf_part[16][4], part[16][4], uv_e8[10];
    uint8_t sw_interp[4][2], comp_inter[5], skip[4], joints[3], sign[2], class0[2][1], fp[2][3], class0_hp[2], hp[2];
    uint8_t classes[2][10], class0_fp[2][2][3], bits[2][10], single_ref[5][2], comp_ref[5], pad58f[17];
    uint8_t coef[4][2][2][6][6][4];
} nvdec_probs;

typedef struct {
    uint8_t kf_y_mode[10][10][9], seg_tree[7], seg_pred[3], kf_uv_mode[10][9], y_mode[4][9], uv_mode[10][9];
    uint8_t kf_partition[16][3], partition[16][3], coef[4][2][2][6][6][3], switchable_interp[4][2], inter_mode[7][3];
    uint8_t intra_inter[4], comp_inter[5], single_ref[5][2], comp_ref[5], tx32[2][3], tx16[2][2], tx8[2][1], skip[3];
    uint8_t joints[3], sign[2], classes[2][10], class0[2][1], bits[2][10], class0_fp[2][2][3], fp[2][3], class0_hp[2],
        hp[2];
} vp9_probs;

typedef struct {
    uint32_t y_mode[4][10], uv_mode[10][10], partition[16][4], coef[4][2][2][6][6][4], eob[4][2][2][6][6];
    uint32_t sw_interp[4][3], inter_mode[7][4], intra_inter[4][2], comp_inter[5][2], single_ref[5][2][2],
        comp_ref[5][2];
    uint32_t tx32[2][4], tx16[2][3], tx8[2][2], skip[3][2], joints[4], sign[2][2], classes[2][11], class0[2][2];
    uint32_t bits[2][10][2], class0_fp[2][2][4], fp[2][4], class0_hp[2][2], hp[2][2];
} vp9_counts;

typedef struct {
    uint32_t inter_mode[7][3][2], y_mode[4][10], uv_mode[10][10], partition[16][4], sw_interp[4][3];
    uint32_t intra_inter[4][2], comp_inter[5][2], single_ref[5][2][2], comp_ref[5][2], tx32[2][4], tx16[2][3];
    uint32_t tx8[2][2], skip[3][2], joints[4], sign[2][2], classes[2][11], class0[2][2], bits[2][10][2];
    uint32_t class0_fp[2][2][4], fp[2][4], class0_hp[2][2], hp[2][2], coef[4][2][2][6][6][4], eob[4][2][2][6][6];
} nvdec_counts;

static_assert(sizeof(nvdec_probs) == S2A_NVDEC_ENTROPY_SIZE, "NVDEC EntropyProbs");
static_assert(sizeof(vp9_probs) == 3087, "Vp9EntropyProbs");
static_assert(sizeof(vp9_counts) == 13208, "Vp9BackwardUpdates");
static_assert(sizeof(nvdec_counts) == S2A_NVDEC_COUNTS_SIZE, "BackwardUpdates (guest layout)");

void s2a_entropy_convert(const uint8_t *in, uint8_t *out)
{
    nvdec_probs g;
    vp9_probs f;
    memcpy(&g, in, sizeof g);
    memset(&f, 0, sizeof f);
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
        {
            memcpy(f.kf_y_mode[i][j], g.kf_y_e0_7[i][j], 8);
            f.kf_y_mode[i][j][8] = g.kf_y_e8[i][j];
        }
    memcpy(f.seg_tree, g.seg_tree, 7);
    memcpy(f.seg_pred, g.seg_pred, 3);
    for (int i = 0; i < 7; i++) memcpy(f.inter_mode[i], g.inter_mode[i], 3);
    memcpy(f.intra_inter, g.intra_inter, 4);
    for (int i = 0; i < 10; i++)
    {
        memcpy(f.kf_uv_mode[i], g.kf_uv_e0_7[i], 8);
        f.kf_uv_mode[i][8] = g.kf_uv_e8[i];
        memcpy(f.uv_mode[i], g.uv_e0_7[i], 8);
        f.uv_mode[i][8] = g.uv_e8[i];
    }
    memcpy(f.tx8, g.tx8, sizeof f.tx8);
    memcpy(f.tx16, g.tx16, sizeof f.tx16);
    memcpy(f.tx32, g.tx32, sizeof f.tx32);
    for (int i = 0; i < 4; i++)
    {
        memcpy(f.y_mode[i], g.y_e0_7[i], 8);
        f.y_mode[i][8] = g.y_e8[i];
    }
    for (int i = 0; i < 16; i++)
    {
        memcpy(f.kf_partition[i], g.kf_part[i], 3);
        memcpy(f.partition[i], g.part[i], 3);
    }
    memcpy(f.switchable_interp, g.sw_interp, sizeof f.switchable_interp);
    memcpy(f.comp_inter, g.comp_inter, 5);
    memcpy(f.skip, g.skip, 3);
    memcpy(f.joints, g.joints, 3);
    memcpy(f.sign, g.sign, 2);
    memcpy(f.class0, g.class0, sizeof f.class0);
    memcpy(f.fp, g.fp, sizeof f.fp);
    memcpy(f.class0_hp, g.class0_hp, 2);
    memcpy(f.hp, g.hp, 2);
    memcpy(f.classes, g.classes, sizeof f.classes);
    memcpy(f.class0_fp, g.class0_fp, sizeof f.class0_fp);
    memcpy(f.bits, g.bits, sizeof f.bits);
    memcpy(f.single_ref, g.single_ref, sizeof f.single_ref);
    memcpy(f.comp_ref, g.comp_ref, 5);
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 2; b++)
            for (int c = 0; c < 2; c++)
                for (int d = 0; d < 6; d++)
                    for (int e = 0; e < 6; e++) memcpy(f.coef[a][b][c][d][e], g.coef[a][b][c][d][e], 3);
    memcpy(out, &f, sizeof f);
}

size_t s2a_counts_to_guest(const uint8_t *in, uint8_t *out)
{
    vp9_counts c;
    nvdec_counts g;
    memcpy(&c, in, sizeof c);
    for (int i = 0; i < 7; i++)
    {
        const uint32_t *m = c.inter_mode[i];
        g.inter_mode[i][0][0] = m[2];
        g.inter_mode[i][0][1] = m[0] + m[1] + m[3];
        g.inter_mode[i][1][0] = m[0];
        g.inter_mode[i][1][1] = m[1] + m[3];
        g.inter_mode[i][2][0] = m[1];
        g.inter_mode[i][2][1] = m[3];
    }
#define CP(x) memcpy(g.x, c.x, sizeof g.x)
    CP(y_mode); CP(uv_mode); CP(partition); CP(sw_interp); CP(intra_inter); CP(comp_inter); CP(single_ref);
    CP(comp_ref); CP(tx32); CP(tx16); CP(tx8); CP(skip); CP(joints); CP(sign); CP(classes); CP(class0); CP(bits);
    CP(class0_fp); CP(fp); CP(class0_hp); CP(hp); CP(coef); CP(eob);
#undef CP
    memcpy(out, &g, sizeof g);
    return sizeof g;
}

// ---- bit writer and header ----

typedef struct {
    uint8_t *p;
    size_t cap, bit;
} bw;

static void put(bw *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, w->bit++)
    {
        size_t byte = w->bit >> 3;
        if (byte >= w->cap) continue;
        if ((w->bit & 7) == 0) w->p[byte] = 0;
        if ((v >> i) & 1) w->p[byte] |= (uint8_t)(0x80 >> (w->bit & 7));
    }
}

static void put_signed(bw *w, int v, int n)   // magnitude, then sign (vpx_rb_read_signed_literal)
{
    put(w, (uint32_t)(v < 0 ? -v : v), n);
    put(w, v < 0, 1);
}

static void tile_range(int width, int *min_log2, int *max_log2)
{
    const int mi_cols = (width + 7) >> 3;
    const int sb64_cols = (mi_cols + 7) >> 3;
    int mn = 0, mx = 1;
    while ((64 << mn) < sb64_cols) mn++;
    while ((sb64_cols >> mx) >= 4) mx++;
    *min_log2 = mn;
    *max_log2 = mx - 1;
}

void s2a_slots_reset(s2a_slots *slots) { memset(slots, 0, sizeof *slots); }

static int find_slot(const s2a_slots *s, uint32_t luma, uint32_t chroma, int w, int h)
{
    for (int i = 0; i < 8; i++)
        if (s->s[i].valid && s->s[i].luma == luma && s->s[i].chroma == chroma && s->s[i].w == w && s->s[i].h == h)
            return i;
    return -1;
}

// ---- compressed header (spec §6.3): C mirror of compat/tree/.../Vp9CompressedHeader.cs ----
// (section offsets live in hdr.h — the 3087-byte supplied-probs layout contract)

typedef struct {
    uint8_t *dst;
    size_t cap, pos;
    uint32_t low, range;
    int count, error;
} bcw;

static void bcw_init(bcw *w, uint8_t *dst, size_t cap)
{
    w->dst = dst; w->cap = cap; w->pos = 0;
    w->low = 0; w->range = 255; w->count = -24; w->error = 0;
}

static void bcw_write(bcw *w, int bit, int prob)
{
    uint32_t split = 1 + (((w->range - 1) * (uint32_t)prob) >> 8);
    uint32_t range = split;
    if (bit) { w->low += split; range = w->range - split; }
    int shift = 0;
    while (range < 128) { range <<= 1; shift++; }   // vpx_norm
    w->count += shift;
    if (w->count >= 0)
    {
        int offset = shift - w->count;
        if (!w->error)
        {
            if (offset >= 1 && ((w->low << (offset - 1)) & 0x80000000u))
            {
                int x = (int)w->pos - 1;
                while (x >= 0 && w->dst[x] == 0xff) w->dst[x--] = 0;
                if (x >= 0) w->dst[x]++;
            }
            if (w->pos < w->cap) w->dst[w->pos++] = (uint8_t)((w->low >> (24 - offset)) & 0xff);
            else w->error = 1;
        }
        w->low = (w->low << offset) & 0xffffffu;
        shift = w->count;
        w->count -= 8;
    }
    w->low <<= shift;
    w->range = range;
}

static void bcw_bit(bcw *w, int bit) { bcw_write(w, bit, 128); }

static void bcw_literal(bcw *w, int v, int bits)
{
    for (int i = bits - 1; i >= 0; i--) bcw_bit(w, (v >> i) & 1);
}

static int bcw_stop(bcw *w)
{
    for (int i = 0; i < 32; i++) bcw_bit(w, 0);
    return w->error ? -1 : (int)w->pos;
}

static const uint8_t map_table[254] = {
     20,  21,  22,  23,  24,  25,   0,  26,  27,  28,  29,  30,  31,  32,  33,
     34,  35,  36,  37,   1,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
     48,  49,   2,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,
      3,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,   4,  74,
     75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,   5,  86,  87,  88,
     89,  90,  91,  92,  93,  94,  95,  96,  97,   6,  98,  99, 100, 101, 102,
    103, 104, 105, 106, 107, 108, 109,   7, 110, 111, 112, 113, 114, 115, 116,
    117, 118, 119, 120, 121,   8, 122, 123, 124, 125, 126, 127, 128, 129, 130,
    131, 132, 133,   9, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144,
    145,  10, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157,  11,
    158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169,  12, 170, 171,
    172, 173, 174, 175, 176, 177, 178, 179, 180, 181,  13, 182, 183, 184, 185,
    186, 187, 188, 189, 190, 191, 192, 193,  14, 194, 195, 196, 197, 198, 199,
    200, 201, 202, 203, 204, 205,  15, 206, 207, 208, 209, 210, 211, 212, 213,
    214, 215, 216, 217,  16, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227,
    228, 229,  17, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
     18, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253,  19,
};

static int recenter_nonneg(int v, int m)
{
    if (v > (m << 1)) return v;
    return v >= m ? (v - m) << 1 : ((m - v) << 1) - 1;
}

static int remap_prob(int v, int m)
{
    v--; m--;
    int i = (m << 1) <= 255 ? recenter_nonneg(v, m) - 1 : recenter_nonneg(254 - v, 254 - m) - 1;
    return map_table[i];
}

static void encode_uniform(bcw *w, int v)
{
    const int m = (1 << 8) - 191;   // 65 — decode_uniform's cutoff
    if (v < m) bcw_literal(w, v, 7);
    else { bcw_literal(w, m + ((v - m) >> 1), 7); bcw_literal(w, (v - m) & 1, 1); }
}

static void encode_term_subexp(bcw *w, int word)
{
    if (word < 16) { bcw_bit(w, 0); bcw_literal(w, word, 4); }
    else if (word < 32) { bcw_bit(w, 1); bcw_bit(w, 0); bcw_literal(w, word - 16, 4); }
    else if (word < 64) { bcw_bit(w, 1); bcw_bit(w, 1); bcw_bit(w, 0); bcw_literal(w, word - 32, 5); }
    else { bcw_bit(w, 1); bcw_bit(w, 1); bcw_bit(w, 1); encode_uniform(w, word - 64); }
}

static void diff_update(bcw *w, int newp, int oldp)
{
    bcw_write(w, newp != oldp, 252);
    if (newp != oldp) encode_term_subexp(w, remap_prob(newp, oldp));
}

static void mv_update(bcw *w, int newp, int oldp)
{
    bcw_write(w, newp != oldp, 252);
    if (newp != oldp) bcw_literal(w, newp >> 1, 7);
}

static void diff_range(bcw *w, const uint8_t *probs, const uint8_t *def, int off, int n)
{
    for (int i = 0; i < n; i++) diff_update(w, probs[off + i], def[off + i]);
}

static void mv_range(bcw *w, const uint8_t *probs, const uint8_t *def, int off, int n)
{
    for (int i = 0; i < n; i++) mv_update(w, probs[off + i], def[off + i]);
}

static int region_equal(const uint8_t *probs, const uint8_t *def, int off, int n)
{
    return memcmp(probs + off, def + off, (size_t)n) == 0;
}

static int mv_even_diff(const uint8_t *probs, const uint8_t *def, int off, int n)
{
    for (int i = 0; i < n; i++)
        if (probs[off + i] != def[off + i] && (probs[off + i] & 1) == 0) return 1;
    return 0;
}

static int compound_allowed(const s2a_pic *p)
{
    return p->sign_bias[2] != p->sign_bias[1] || p->sign_bias[3] != p->sign_bias[1];
}

const char *s2a_compressed_check(const s2a_pic *p, const uint8_t *probs, const uint8_t *defaults)
{
    const int key = (p->flags & S2A_KEY) != 0;
    const int intra_only = !key && (p->flags & S2A_INTRA_ONLY);
    // error_resilient only gates the compressed-header path: the repack emits error_resilient_mode=0
    // and a resilient job derives use_prev_frame_mvs differently than the stream intended. libvpx's
    // out-of-band path carries no such header, so the flag must not gate it (Vp9CompressedHeader.Check).
    if (p->flags & S2A_ERROR_RES) return "error_resilient";
    if (!key && !region_equal(probs, defaults, S2A_OFF_UV, 90)) return "uv_mode probs";
    if (intra_only && (!region_equal(probs, defaults, S2A_OFF_YMODE, 36) ||
                       !region_equal(probs, defaults, S2A_OFF_PARTITION, 48)))
        return "intra probs";
    if (key || intra_only) return NULL;
    if (!compound_allowed(p) && p->ref_mode != 0) return "reference_mode";
    if (mv_even_diff(probs, defaults, S2A_OFF_JOINTS, 3) || mv_even_diff(probs, defaults, S2A_OFF_SIGN, 2) ||
        mv_even_diff(probs, defaults, S2A_OFF_CLASSES, 20) || mv_even_diff(probs, defaults, S2A_OFF_CLASS0, 2) ||
        mv_even_diff(probs, defaults, S2A_OFF_BITS, 20) || mv_even_diff(probs, defaults, S2A_OFF_CLASS0FP, 12) ||
        mv_even_diff(probs, defaults, S2A_OFF_FP, 6) ||
        (p->allow_hp && (mv_even_diff(probs, defaults, S2A_OFF_CLASS0HP, 2) || mv_even_diff(probs, defaults, S2A_OFF_HP, 2))))
        return "mv probs";
    return NULL;
}

int s2a_compressed_write(const s2a_pic *p, const uint8_t *probs, const uint8_t *defaults,
                         uint8_t *dst, size_t cap)
{
    bcw w;
    bcw_init(&w, dst, cap);
    bcw_bit(&w, 0);   // vpx_start_encode's leading 0
    const int tx = p->lossless ? 0 : p->tx_mode;

    if (!p->lossless)
    {
        bcw_literal(&w, tx > 3 ? 3 : tx, 2);
        if (tx >= 3) bcw_bit(&w, tx == 4);
    }
    if (tx == 4)
    {
        diff_range(&w, probs, defaults, S2A_OFF_TX8, 2);
        diff_range(&w, probs, defaults, S2A_OFF_TX16, 4);
        diff_range(&w, probs, defaults, S2A_OFF_TX32, 6);
    }
    for (int t = 0, maxt = tx < 3 ? tx : 3; t <= maxt; t++)
    {
        int g = S2A_OFF_COEF + t * 432, upd = 0;
        for (int i = 0; i < 4 && !upd; i++)
            for (int b = 0; b < 6 && !upd; b++)
                if (!region_equal(probs, defaults, g + (i * 6 + b) * 18, (b == 0 ? 3 : 6) * 3)) upd = 1;
        bcw_bit(&w, upd);
        if (upd)
            for (int i = 0; i < 4; i++)
                for (int b = 0; b < 6; b++)
                    diff_range(&w, probs, defaults, g + (i * 6 + b) * 18, (b == 0 ? 3 : 6) * 3);
    }
    diff_range(&w, probs, defaults, S2A_OFF_SKIP, 3);

    const int key = (p->flags & S2A_KEY) != 0;
    const int intra_only = !key && (p->flags & S2A_INTRA_ONLY);
    if (!key && !intra_only)
    {
        diff_range(&w, probs, defaults, S2A_OFF_INTERMODE, 21);
        if (p->interp == 4) diff_range(&w, probs, defaults, S2A_OFF_SWINTERP, 8);
        diff_range(&w, probs, defaults, S2A_OFF_INTRAINTER, 4);

        if (compound_allowed(p))
        {
            bcw_bit(&w, p->ref_mode != 0);
            if (p->ref_mode != 0) bcw_bit(&w, p->ref_mode == 2);
        }
        if (p->ref_mode == 2) diff_range(&w, probs, defaults, S2A_OFF_COMPINTER, 5);
        if (p->ref_mode != 1) diff_range(&w, probs, defaults, S2A_OFF_SINGLEREF, 10);
        if (p->ref_mode != 0) diff_range(&w, probs, defaults, S2A_OFF_COMPREF, 5);

        diff_range(&w, probs, defaults, S2A_OFF_YMODE, 36);
        diff_range(&w, probs, defaults, S2A_OFF_PARTITION, 48);

        mv_range(&w, probs, defaults, S2A_OFF_JOINTS, 3);
        for (int c = 0; c < 2; c++)
        {
            mv_range(&w, probs, defaults, S2A_OFF_SIGN + c, 1);
            mv_range(&w, probs, defaults, S2A_OFF_CLASSES + c * 10, 10);
            mv_range(&w, probs, defaults, S2A_OFF_CLASS0 + c, 1);
            mv_range(&w, probs, defaults, S2A_OFF_BITS + c * 10, 10);
        }
        for (int c = 0; c < 2; c++)
        {
            mv_range(&w, probs, defaults, S2A_OFF_CLASS0FP + c * 6, 6);
            mv_range(&w, probs, defaults, S2A_OFF_FP + c * 3, 3);
        }
        if (p->allow_hp)
            for (int c = 0; c < 2; c++)
            {
                mv_range(&w, probs, defaults, S2A_OFF_CLASS0HP + c, 1);
                mv_range(&w, probs, defaults, S2A_OFF_HP + c, 1);
            }
    }
    return bcw_stop(&w);
}

static const char *frame_prepare(const s2a_pic *p, const s2a_surfaces *sf, s2a_slots *slots,
                                 int ref_idx[3], int *target, int *min_log2, int *max_log2)
{
    const int key = (p->flags & S2A_KEY) != 0;
    const int intra_only = !key && (p->flags & S2A_INTRA_ONLY);
    const int w = p->size_w[3], h = p->size_h[3];
    const unsigned bit_depth = (p->surface_params >> 1) & 0xf;

    // §2 checks
    if (p->seg_enabled) return "segmentation";
    if (bit_depth != 0 && bit_depth != 8) return "bit depth";
    if (w < 1 || h < 1 || w > 8192 || h > 8192) return "frame size";
    if (p->tx_mode > 4) return "tx_mode";
    if (p->ref_mode > 2) return "reference_mode";
    if (p->interp > 4) return "interp_filter";
    if (p->filter_level > 63 || p->sharpness > 7) return "loop filter level";
    if (p->y_dc > 15 || p->uv_dc > 15 || p->uv_ac > 15) return "delta_q > 15";
    if ((p->lossless != 0) != (p->base_q == 0 && p->y_dc == 0 && p->uv_dc == 0 && p->uv_ac == 0)) return "lossless";
    for (int i = 0; i < 4; i++)
        if (p->ref_deltas[i] > 63 || p->ref_deltas[i] < -63) return "loop filter delta";
    for (int i = 0; i < 2; i++)
        if (p->mode_deltas[i] > 63 || p->mode_deltas[i] < -63) return "loop filter delta";
    for (int i = 1; i < 4; i++)
        if (p->sign_bias[i] != 0 && p->sign_bias[i] != 1) return "sign bias";
    tile_range(w, min_log2, max_log2);
    if (p->log2_tile_cols < *min_log2 || p->log2_tile_cols > *max_log2) return "tile columns";
    if (p->log2_tile_rows > 2) return "tile rows";

    // §3 slots
    slots->now++;
    int referenced[8] = { 0 };
    if (!key && !intra_only)
        for (int r = 0; r < 3; r++)
        {
            ref_idx[r] = find_slot(slots, sf->luma[r], sf->chroma[r], p->size_w[r], p->size_h[r]);
            if (ref_idx[r] < 0) return "reference not in a slot";
            referenced[ref_idx[r]] = 1;
            slots->s[ref_idx[r]].last_use = slots->now;
        }
    if (key)
        *target = -1;   // all 8
    else
    {
        // Target slot, first rule that applies: (1) a valid slot already holding the output offset and not
        // referenced; (2) an invalid slot; (3) the least recently used valid slot not referenced, lowest index on ties.
        int t = -1;
        for (int i = 0; i < 8 && t < 0; i++)
            if (slots->s[i].valid && slots->s[i].luma == sf->luma[3] && !referenced[i]) t = i;
        for (int i = 0; i < 8 && t < 0; i++)
            if (!slots->s[i].valid) t = i;
        if (t < 0)
        {
            uint64_t best = UINT64_MAX;
            for (int i = 0; i < 8; i++)
                if (slots->s[i].valid && !referenced[i] && slots->s[i].last_use < best)
                {
                    best = slots->s[i].last_use;
                    t = i;
                }
        }
        for (int r = 0; r < 3 && !intra_only; r++)
            if (sf->luma[r] == sf->luma[3]) return "decoding in place";
        if (t < 0) return "no free slot";
        *target = t;
    }
    return NULL;
}

// Uncompressed header bits + compressed bytes + tiles. *len receives the total; returns NULL or the reason.
// mc: emit the byte-level fixups a real decoder requires (interp literal remap, tail-bit mask, marker
// pad byte). The libvpx backend consumed the pre-fixup byte shape since this file existed — its emit
// must keep producing those exact bytes (mc=0, Vp9NativeHeader.WriteUncompressed).
static const char *frame_emit(const s2a_pic *p, const int ref_idx[3], int target, int min_log2, int max_log2,
                              int mc, const uint8_t *compressed, size_t n_compressed,
                              const uint8_t *tiles, size_t tiles_size, uint8_t *dst, size_t cap, size_t *len)
{
    const int key = (p->flags & S2A_KEY) != 0;
    const int intra_only = !key && (p->flags & S2A_INTRA_ONLY);
    const int w = p->size_w[3], h = p->size_h[3];

    bw b = { dst, cap, 0 };
    put(&b, 2, 2);                 // frame_marker
    put(&b, 0, 2);                 // profile 0
    put(&b, 0, 1);                 // show_existing_frame
    put(&b, key ? 0 : 1, 1);       // frame_type
    put(&b, intra_only ? 0 : 1, 1); // show_frame
    put(&b, 0, 1);                 // error_resilient_mode
    if (key)
    {
        put(&b, 0x49, 8); put(&b, 0x83, 8); put(&b, 0x42, 8);
        put(&b, 0, 3);             // color_space (unknown)
        put(&b, 0, 1);             // color_range
        put(&b, (uint32_t)(w - 1), 16); put(&b, (uint32_t)(h - 1), 16);
        put(&b, 0, 1);             // render_and_frame_size_different
    }
    else
    {
        if (intra_only) put(&b, 1, 1);   // intra_only (show_frame is 0)
        put(&b, 0, 2);                   // reset_frame_context
        if (intra_only)
        {
            put(&b, 0x49, 8); put(&b, 0x83, 8); put(&b, 0x42, 8);
            put(&b, 1u << target, 8);    // refresh_frame_flags
            put(&b, (uint32_t)(w - 1), 16); put(&b, (uint32_t)(h - 1), 16);
            put(&b, 0, 1);
        }
        else
        {
            put(&b, 1u << target, 8);
            for (int r = 0; r < 3; r++)
            {
                put(&b, (uint32_t)ref_idx[r], 3);
                put(&b, (uint32_t)p->sign_bias[r + 1], 1);
            }
            put(&b, 0, 3);               // found_ref x3
            put(&b, (uint32_t)(w - 1), 16); put(&b, (uint32_t)(h - 1), 16);
            put(&b, 0, 1);
            put(&b, p->allow_hp != 0, 1);
            put(&b, p->interp == 4, 1);
            // p->interp is the bitstream literal already (NVDEC pic[121]; managed Decoder.cs runs
            // LiteralToFilter itself) — write it verbatim on both paths. An earlier revision remapped
            // enum→literal here, which double-swapped lit∈{0,1} streams into a wrong declaration.
            if (p->interp != 4)
                put(&b, (uint8_t)p->interp, 2);
        }
    }
    put(&b, 0, 1);                 // refresh_frame_context
    put(&b, 0, 1);                 // frame_parallel_decoding_mode
    put(&b, 0, 2);                 // frame_context_idx
    put(&b, p->filter_level, 6);
    put(&b, p->sharpness, 3);
    put(&b, p->mode_ref_delta_enabled != 0, 1);
    if (p->mode_ref_delta_enabled)
    {
        put(&b, 1, 1);             // mode_ref_delta_update
        for (int i = 0; i < 4; i++) { put(&b, 1, 1); put_signed(&b, p->ref_deltas[i], 6); }
        for (int i = 0; i < 2; i++) { put(&b, 1, 1); put_signed(&b, p->mode_deltas[i], 6); }
    }
    put(&b, p->base_q, 8);
    const int dq[3] = { p->y_dc, p->uv_dc, p->uv_ac };
    for (int i = 0; i < 3; i++)
    {
        put(&b, dq[i] != 0, 1);
        if (dq[i]) put_signed(&b, dq[i], 4);
    }
    put(&b, 0, 1);                 // segmentation_enabled
    for (int k = min_log2; k < max_log2; k++)
    {
        put(&b, p->log2_tile_cols > k, 1);
        if (p->log2_tile_cols <= k) break;
    }
    put(&b, p->log2_tile_rows != 0, 1);
    if (p->log2_tile_rows) put(&b, p->log2_tile_rows == 2, 1);
    put(&b, (uint32_t)n_compressed, 16);   // header_size_in_bytes
    size_t n = (b.bit + 7) >> 3;
    if (n + n_compressed + tiles_size > cap) return "frame buffer too small";
    if (mc && (b.bit & 7)) dst[n - 1] &= (uint8_t)(0xff << (8 - (b.bit & 7)));
    memcpy(dst + n, compressed, n_compressed);
    memcpy(dst + n + n_compressed, tiles, tiles_size);
    size_t len0 = n + n_compressed + tiles_size;
    // A packet whose last byte matches the superframe-index marker (110xxxxx) is reparsed by
    // decoders as a multi-frame index — libvpx splits it into a phantom trailing frame. The spec
    // guard: append a zero byte so the tail can never look like the index marker. MC-path only —
    // the libvpx packet goes to the hook as a byte blob and must not grow a tail byte.
    if (mc && (dst[len0 - 1] & 0xe0) == 0xc0)
    {
        if (len0 + 1 > cap) return "frame buffer too small";
        dst[len0++] = 0;
    }
    *len = len0;
    return NULL;
}

const char *s2a_build_frame(const s2a_pic *p, const s2a_surfaces *sf, s2a_slots *slots, const uint8_t *tiles,
                            size_t tiles_size, uint8_t *dst, size_t cap, size_t *len, int *target)
{
    int ref_idx[3] = { 0, 0, 0 }, min_log2, max_log2;
    const char *err = frame_prepare(p, sf, slots, ref_idx, target, &min_log2, &max_log2);
    if (err) return err;
    static const uint8_t standin[1] = { 0 };
    return frame_emit(p, ref_idx, *target, min_log2, max_log2, 0, standin, 1, tiles, tiles_size, dst, cap, len);
}

const char *s2a_build_frame2(const s2a_pic *p, const s2a_surfaces *sf, s2a_slots *slots,
                             const uint8_t *probs, const uint8_t *defaults, const uint8_t *tiles,
                             size_t tiles_size, uint8_t *dst, size_t cap, size_t *len, int *target)
{
    int ref_idx[3] = { 0, 0, 0 }, min_log2, max_log2;
    const char *err = frame_prepare(p, sf, slots, ref_idx, target, &min_log2, &max_log2);
    if (err) return err;
    uint8_t scratch[3456];
    int nc = s2a_compressed_write(p, probs, defaults, scratch, sizeof scratch);
    if (nc < 0) return "compressed header overflow";
    return frame_emit(p, ref_idx, *target, min_log2, max_log2, 1, scratch, (size_t)nc, tiles, tiles_size,
                      dst, cap, len);
}

void s2a_slots_commit(s2a_slots *slots, const s2a_pic *p, const s2a_surfaces *sf, int target)
{
    const s2a_slot out = { sf->luma[3], sf->chroma[3], p->size_w[3], p->size_h[3], 1, slots->now };
    for (int i = 0; i < 8; i++)
    {
        if (target < 0 || i == target)
            slots->s[i] = out;
        else if (slots->s[i].luma == sf->luma[3])
            slots->s[i].valid = 0;
    }
}
