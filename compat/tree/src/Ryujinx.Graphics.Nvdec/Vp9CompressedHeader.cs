using System;
using System.Numerics;

namespace Ryujinx.Graphics.Nvdec
{
    // S2Arm: the real VP9 compressed header for a repacked NVDEC job (bitstream spec §6.3). It replaces the
    // one-byte stand-in Vp9NativeHeader.Write used to append: the repack pins the decoder's context slot 0 at
    // spec defaults (key frame resets all contexts; refresh_frame_context=0 means *fc is only ever a copy and
    // frame_contexts[0] never drifts), so every diff update here is spec-default -> the job's supplied table.
    //
    // Emission order is exactly the decoder's read order — 出处：libvpx 1.17.0 (pinned, BSD-3-Clause)
    // vp9/decoder/vp9_decodeframe.c `read_compressed_header`、vp9/common/vp9_entropymode.h 默认表；
    // C 镜像 android/codecs/vpxbench/hdr.c 同款，hdrcmp 金样测字节级一致：
    //   tx_mode | tx probs (TX_MODE_SELECT) | coef probs | skip |
    //   [inter only] inter_mode | interp probs (SWITCHABLE) | intra_inter | reference_mode | ref probs |
    //   y_mode | partition | mv probs
    // use_prev_frame_mvs is not a header field — the decoder derives it from uncompressed state, which an
    // unbroken native segment keeps consistent with the driver's UsePrevInFindMvRefs flag.
    static class Vp9CompressedHeader
    {
        public const int ProbsSize = 3087;          // Vp9EntropyProbs
        public const int MaxBytes = 3456;           // every prob updated, worst-case term-subexp coding

        // Field offsets inside the 3087-byte supplied-probs blob (vp9_probs layout; the fixture test pins
        // these against Marshal.OffsetOf<Vp9EntropyProbs>).
        private const int OffYMode = 1000;          // [4][9]
        private const int OffUvMode = 1036;         // [10][9] — ctx-only, no update slots exist
        private const int OffPartition = 1174;      // [16][3]
        private const int OffCoef = 1222;           // [4][2][2][6][6][3], 432 per tx size
        private const int OffSwitchInterp = 2950;   // [4][2]
        private const int OffInterMode = 2958;      // [7][3]
        private const int OffIntraInter = 2979;     // [4]
        private const int OffCompInter = 2983;      // [5]
        private const int OffSingleRef = 2988;      // [5][2]
        private const int OffCompRef = 2998;        // [5]
        private const int OffTx32 = 3003;           // [2][3]
        private const int OffTx16 = 3009;           // [2][2]
        private const int OffTx8 = 3013;            // [2][1]
        private const int OffSkip = 3015;           // [3]
        private const int OffJoints = 3018;         // [3]
        private const int OffSign = 3021;           // [2]
        private const int OffClasses = 3023;        // [2][10]
        private const int OffClass0 = 3043;         // [2][1]
        private const int OffBits = 3045;           // [2][10]
        private const int OffClass0Fp = 3065;       // [2][2][3]
        private const int OffFp = 3077;             // [2][3]
        private const int OffClass0Hp = 3083;       // [2]
        private const int OffHp = 3085;             // [2]

        private const int TxSelect = 4;
        private const int RefSingle = 0, RefCompound = 1, RefSelect = 2;

        // ---- bool-coder writer: mirror of libvpx vpx_writer (24-bit lowvalue, 0x80-carry propagation) ----

        private ref struct BoolWriter
        {
            private readonly Span<byte> _dst;
            private uint _low, _range;
            private int _count;
            public int Pos;
            public bool Error;

            public BoolWriter(Span<byte> dst)
            {
                _dst = dst;
                _low = 0;
                _range = 255;
                _count = -24;
                Pos = 0;
                Error = false;
                Bit(false);                          // vpx_start_encode's leading 0
            }

            public void Bit(bool bit) => Write(bit, 128);

            public void Write(bool bit, int prob)
            {
                uint split = 1 + ((_range - 1) * (uint)prob >> 8);
                uint range = split;
                if (bit)
                {
                    _low += split;
                    range = _range - split;
                }
                int shift = BitOperations.LeadingZeroCount(range) - 24;   // vpx_norm: shifts until 8-bit msb
                range <<= shift;
                _count += shift;
                if (_count >= 0)
                {
                    int offset = shift - _count;                          // always >= 1 while this holds
                    if (!Error)
                    {
                        if (offset >= 1 && (_low << (offset - 1) & 0x8000_0000) != 0)
                        {
                            int x = Pos - 1;
                            while (x >= 0 && _dst[x] == 0xff) _dst[x--] = 0;
                            if (x >= 0) _dst[x]++;
                        }
                        if (Pos < _dst.Length) _dst[Pos++] = (byte)(_low >> (24 - offset) & 0xff);
                        else Error = true;
                    }
                    _low = (_low << offset) & 0xff_ffff;
                    shift = _count;
                    _count -= 8;
                }
                _low <<= shift;
                _range = range;
            }

            public void Literal(int v, int bits)
            {
                for (int i = bits - 1; i >= 0; i--) Bit((v >> i & 1) != 0);
            }

            public int Stop()
            {
                for (int i = 0; i < 32; i++) Bit(false);                  // vpx_stop_encode flush
                return Error ? -1 : Pos;
            }
        }

        // ---- diff_update_prob: flag at probability 252, then subexponential-coded remap index ----

        private static readonly byte[] MapTable =  // spec's prob remap (encoder-side inverse of inv_map_table)
        [
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
        ];

        private static int RecenterNonNeg(int v, int m)
        {
            if (v > (m << 1)) return v;
            return v >= m ? (v - m) << 1 : ((m - v) << 1) - 1;
        }

        private static int RemapProb(int v, int m)   // v = new prob, m = old prob
        {
            v--; m--;
            int i = (m << 1) <= 255 ? RecenterNonNeg(v, m) - 1 : RecenterNonNeg(254 - v, 254 - m) - 1;
            return MapTable[i];
        }

        private static void EncodeUniform(ref BoolWriter w, int v)
        {
            if (v < 65) w.Literal(v, 7);
            else { w.Literal(65 + ((v - 65) >> 1), 7); w.Literal((v - 65) & 1, 1); }
        }

        private static void EncodeTermSubexp(ref BoolWriter w, int word)
        {
            if (word < 16) { w.Bit(false); w.Literal(word, 4); }
            else if (word < 32) { w.Bit(true); w.Bit(false); w.Literal(word - 16, 4); }
            else if (word < 64) { w.Bit(true); w.Bit(true); w.Bit(false); w.Literal(word - 32, 5); }
            else { w.Bit(true); w.Bit(true); w.Bit(true); EncodeUniform(ref w, word - 64); }
        }

        private static void DiffUpdate(ref BoolWriter w, int newP, int oldP)
        {
            w.Write(newP != oldP, 252);
            if (newP != oldP) EncodeTermSubexp(ref w, RemapProb(newP, oldP));
        }

        private static void DiffRange(ref BoolWriter w, ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, int off, int n)
        {
            for (int i = 0; i < n; i++) DiffUpdate(ref w, probs[off + i], defaults[off + i]);
        }

        // update_mv_probs (MV_UPDATE_PROB = 252): update bit, then a 7-bit literal decoded as (v << 1) | 1 —
        // only odd target values are expressible.
        private static void MvUpdate(ref BoolWriter w, int newP, int oldP)
        {
            w.Write(newP != oldP, 252);
            if (newP != oldP) w.Literal(newP >> 1, 7);
        }

        private static void MvRange(ref BoolWriter w, ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, int off, int n)
        {
            for (int i = 0; i < n; i++) MvUpdate(ref w, probs[off + i], defaults[off + i]);
        }

        private static bool Equal(ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, int off, int n)
            => probs.Slice(off, n).SequenceEqual(defaults.Slice(off, n));

        // Sections that take mv updates: joints/sign/classes/class0/bits/class0_fp/fp (+hp when allowed).
        private static bool MvEvenDiff(ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, int off, int n)
        {
            for (int i = 0; i < n; i++)
                if (probs[off + i] != defaults[off + i] && (probs[off + i] & 1) == 0) return true;
            return false;
        }

        private static bool CompoundAllowed(in Vp9NativeInput p)
            => p.SignBias2 != p.SignBias1 || p.SignBias3 != p.SignBias1;

        // Null when every load-bearing probability can be carried by the compressed header; else the reason
        // the job stays on the managed decoder. Sections with no update slot (uv_mode for any shown frame;
        // y_mode/partition for intra-only) must sit at the ctx base — spec defaults. MV updates can only
        // express odd targets; a reference mode above SINGLE is only writable when compound is allowed.
        public static string Check(in Vp9NativeInput p, ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults)
        {
            // The repack always emits error_resilient_mode=0; an error-resilient job would derive
            // use_prev_frame_mvs differently on the hardware side than in the original stream. Scoped here:
            // the libvpx backend has no compressed header to corrupt, so the flag must not gate it.
            if (p.ErrorResilient) return "error_resilient";
            if (probs.Length < ProbsSize || defaults.Length < ProbsSize) return "probs blob";

            bool intraOnly = !p.KeyFrame && p.IntraOnly;
            if (!p.KeyFrame && !Equal(probs, defaults, OffUvMode, 90)) return "uv_mode probs";
            if (intraOnly && (!Equal(probs, defaults, OffYMode, 36) || !Equal(probs, defaults, OffPartition, 48)))
                return "intra probs";

            if (p.KeyFrame || intraOnly) return null;

            if (!CompoundAllowed(in p) && p.ReferenceMode != RefSingle) return "reference_mode";

            if (MvEvenDiff(probs, defaults, OffJoints, 3) || MvEvenDiff(probs, defaults, OffSign, 2) ||
                MvEvenDiff(probs, defaults, OffClasses, 20) || MvEvenDiff(probs, defaults, OffClass0, 2) ||
                MvEvenDiff(probs, defaults, OffBits, 20) || MvEvenDiff(probs, defaults, OffClass0Fp, 12) ||
                MvEvenDiff(probs, defaults, OffFp, 6) ||
                (p.AllowHighPrecisionMv && (MvEvenDiff(probs, defaults, OffClass0Hp, 2) || MvEvenDiff(probs, defaults, OffHp, 2))))
                return "mv probs";

            return null;
        }

        // Writes the compressed header into dst; returns its length, or -1 when the buffer cannot hold it.
        // The caller must have run Check first — every field emitted here is verified expressible there.
        public static int Write(in Vp9NativeInput p, ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, Span<byte> dst)
        {
            var w = new BoolWriter(dst);
            int txMode = p.Lossless ? 0 : p.TxMode;

            if (!p.Lossless)
            {
                w.Literal(txMode > 3 ? 3 : txMode, 2);
                if (txMode >= 3) w.Bit(txMode == TxSelect);
            }

            if (txMode == TxSelect)
            {
                DiffRange(ref w, probs, defaults, OffTx8, 2);
                DiffRange(ref w, probs, defaults, OffTx16, 4);
                DiffRange(ref w, probs, defaults, OffTx32, 6);
            }

            // Coef probs: one group-update bit per tx size up to the largest the mode allows; a set bit is
            // followed by diff updates for every entry the decoder reads (band 0 has only 3 contexts).
            for (int tx = 0, maxTx = Math.Min(txMode, 3); tx <= maxTx; tx++)
            {
                int g = OffCoef + tx * 432;
                bool upd = false;
                for (int i = 0; i < 4 && !upd; i++)              // plane x ref
                {
                    for (int b = 0; b < 6 && !upd; b++)
                    {
                        int e = g + (i * 6 + b) * 18;
                        if (!Equal(probs, defaults, e, (b == 0 ? 3 : 6) * 3)) upd = true;
                    }
                }
                w.Bit(upd);
                if (upd)
                {
                    for (int i = 0; i < 4; i++)
                        for (int b = 0; b < 6; b++)
                            DiffRange(ref w, probs, defaults, g + (i * 6 + b) * 18, (b == 0 ? 3 : 6) * 3);
                }
            }

            DiffRange(ref w, probs, defaults, OffSkip, 3);

            bool intra = p.KeyFrame || p.IntraOnly;
            if (!intra)
            {
                DiffRange(ref w, probs, defaults, OffInterMode, 21);
                if (p.InterpFilter == 4) DiffRange(ref w, probs, defaults, OffSwitchInterp, 8);
                DiffRange(ref w, probs, defaults, OffIntraInter, 4);

                if (CompoundAllowed(in p))
                {
                    w.Bit(p.ReferenceMode != RefSingle);
                    if (p.ReferenceMode != RefSingle) w.Bit(p.ReferenceMode == RefSelect);
                }
                if (p.ReferenceMode == RefSelect) DiffRange(ref w, probs, defaults, OffCompInter, 5);
                if (p.ReferenceMode != RefCompound) DiffRange(ref w, probs, defaults, OffSingleRef, 10);
                if (p.ReferenceMode != RefSingle) DiffRange(ref w, probs, defaults, OffCompRef, 5);

                DiffRange(ref w, probs, defaults, OffYMode, 36);
                DiffRange(ref w, probs, defaults, OffPartition, 48);

                MvRange(ref w, probs, defaults, OffJoints, 3);
                for (int c = 0; c < 2; c++)
                {
                    MvRange(ref w, probs, defaults, OffSign + c, 1);
                    MvRange(ref w, probs, defaults, OffClasses + c * 10, 10);
                    MvRange(ref w, probs, defaults, OffClass0 + c, 1);
                    MvRange(ref w, probs, defaults, OffBits + c * 10, 10);
                }
                for (int c = 0; c < 2; c++)
                {
                    MvRange(ref w, probs, defaults, OffClass0Fp + c * 6, 6);
                    MvRange(ref w, probs, defaults, OffFp + c * 3, 3);
                }
                if (p.AllowHighPrecisionMv)
                {
                    for (int c = 0; c < 2; c++)
                    {
                        MvRange(ref w, probs, defaults, OffClass0Hp + c, 1);
                        MvRange(ref w, probs, defaults, OffHp + c, 1);
                    }
                }
            }

            return w.Stop();
        }
    }
}
