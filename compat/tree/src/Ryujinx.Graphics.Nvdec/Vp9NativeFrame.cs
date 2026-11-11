using System;

namespace Ryujinx.Graphics.Nvdec
{
    // S2Arm: the uncompressed VP9 header and reference-slot map for one NVDEC job decoded by libvpx
    // (docs/设计_原生VP9/design_phase2_B.md §2-§3). Same rules as android/codecs/vpxbench/hdr.c, which the tests compare
    // against byte for byte. No dependencies beyond System, so tests compile this file directly.

    // The PictureInfo fields the header needs.
    struct Vp9NativeInput
    {
        public int LastWidth, LastHeight, GoldenWidth, GoldenHeight, AltWidth, AltHeight, Width, Height;
        public bool KeyFrame, IntraOnly, ErrorResilient;
        public int SignBias1, SignBias2, SignBias3;
        public int FilterLevel, Sharpness, BaseQIndex, YDcDeltaQ, UvDcDeltaQ, UvAcDeltaQ;
        public bool Lossless;
        public int TxMode, ReferenceMode, InterpFilter;
        public bool AllowHighPrecisionMv;
        public int Log2TileCols, Log2TileRows;
        public bool SegmentationEnabled;
        public bool ModeRefDeltaEnabled;
        public int RefDelta0, RefDelta1, RefDelta2, RefDelta3, ModeDelta0, ModeDelta1;
        public int BitDepth;                                  // 0 = not set (T210)
        public uint LastLuma, LastChroma, GoldenLuma, GoldenChroma, AltLuma, AltChroma, OutLuma, OutChroma;
    }

    sealed class Vp9SlotMap
    {
        private struct Slot
        {
            public uint Luma, Chroma;
            public int Width, Height;
            public bool Valid;
            public ulong LastUse;
        }

        private readonly Slot[] _slots = new Slot[8];
        private ulong _now;

        public void Reset()
        {
            Array.Clear(_slots);
            _now = 0;
        }

        private int Find(uint luma, uint chroma, int w, int h)
        {
            for (int i = 0; i < 8; i++)
            {
                ref Slot s = ref _slots[i];
                if (s.Valid && s.Luma == luma && s.Chroma == chroma && s.Width == w && s.Height == h)
                {
                    return i;
                }
            }

            return -1;
        }

        // Resolves the three references and the slot the output refreshes (-1 = all 8, key frame). Returns null or the
        // reason the job must go to the managed decoder.
        public string Resolve(in Vp9NativeInput p, Span<int> refIdx, out int target)
        {
            target = -1;
            _now++;

            if (p.KeyFrame)
            {
                return null;
            }

            Span<bool> referenced = stackalloc bool[8];

            if (!p.IntraOnly)
            {
                ReadOnlySpan<uint> luma = [p.LastLuma, p.GoldenLuma, p.AltLuma];
                ReadOnlySpan<uint> chroma = [p.LastChroma, p.GoldenChroma, p.AltChroma];
                ReadOnlySpan<int> w = [p.LastWidth, p.GoldenWidth, p.AltWidth];
                ReadOnlySpan<int> h = [p.LastHeight, p.GoldenHeight, p.AltHeight];

                for (int r = 0; r < 3; r++)
                {
                    refIdx[r] = Find(luma[r], chroma[r], w[r], h[r]);

                    if (refIdx[r] < 0)
                    {
                        return "reference not in a slot";
                    }

                    referenced[refIdx[r]] = true;
                    _slots[refIdx[r]].LastUse = _now;
                }

                for (int r = 0; r < 3; r++)
                {
                    if (luma[r] == p.OutLuma)
                    {
                        return "decoding in place";
                    }
                }
            }

            // Target slot, first rule that applies: (1) a valid slot already holding the output offset and not referenced;
            // (2) an invalid slot; (3) the least recently used valid slot not referenced, lowest index on ties.
            int t = -1;

            for (int i = 0; i < 8 && t < 0; i++)
            {
                if (_slots[i].Valid && _slots[i].Luma == p.OutLuma && !referenced[i])
                {
                    t = i;
                }
            }

            for (int i = 0; i < 8 && t < 0; i++)
            {
                if (!_slots[i].Valid)
                {
                    t = i;
                }
            }

            if (t < 0)
            {
                ulong best = ulong.MaxValue;

                for (int i = 0; i < 8; i++)
                {
                    if (_slots[i].Valid && !referenced[i] && _slots[i].LastUse < best)
                    {
                        best = _slots[i].LastUse;
                        t = i;
                    }
                }
            }

            if (t < 0)
            {
                return "no free slot";
            }

            target = t;
            return null;
        }

        // After a successful native decode: the target (or all 8 on a key frame) holds the output; any other slot with the
        // same luma offset no longer matches guest memory.
        public void Commit(in Vp9NativeInput p, int target)
        {
            Slot output = new() { Luma = p.OutLuma, Chroma = p.OutChroma, Width = p.Width, Height = p.Height, Valid = true, LastUse = _now };

            for (int i = 0; i < 8; i++)
            {
                if (target < 0 || i == target)
                {
                    _slots[i] = output;
                }
                else if (_slots[i].Luma == p.OutLuma)
                {
                    _slots[i].Valid = false;
                }
            }
        }
    }

    static class Vp9NativeHeader
    {
        // The §2 checks: null when libvpx can represent the job, else the reason.
        public static string Check(in Vp9NativeInput p)
        {
            if (p.SegmentationEnabled) return "segmentation";
            if (p.BitDepth != 0 && p.BitDepth != 8) return "bit depth";
            if (p.Width < 1 || p.Height < 1 || p.Width > 8192 || p.Height > 8192) return "frame size";
            if (p.TxMode is < 0 or > 4) return "tx_mode";
            if (p.ReferenceMode is < 0 or > 2) return "reference_mode";
            if (p.InterpFilter is < 0 or > 4) return "interp_filter";
            if (p.FilterLevel is < 0 or > 63 || p.Sharpness is < 0 or > 7) return "loop filter level";
            if (p.YDcDeltaQ is < 0 or > 15 || p.UvDcDeltaQ is < 0 or > 15 || p.UvAcDeltaQ is < 0 or > 15) return "delta_q";
            if (p.Lossless != (p.BaseQIndex == 0 && p.YDcDeltaQ == 0 && p.UvDcDeltaQ == 0 && p.UvAcDeltaQ == 0)) return "lossless";
            if (Math.Abs(p.RefDelta0) > 63 || Math.Abs(p.RefDelta1) > 63 || Math.Abs(p.RefDelta2) > 63 || Math.Abs(p.RefDelta3) > 63 ||
                Math.Abs(p.ModeDelta0) > 63 || Math.Abs(p.ModeDelta1) > 63) return "loop filter delta";
            if (p.SignBias1 is < 0 or > 1 || p.SignBias2 is < 0 or > 1 || p.SignBias3 is < 0 or > 1) return "sign bias";
            TileRange(p.Width, out int min, out int max);
            if (p.Log2TileCols < min || p.Log2TileCols > max) return "tile columns";
            if (p.Log2TileRows is < 0 or > 2) return "tile rows";
            return null;
        }

        public static void TileRange(int width, out int minLog2, out int maxLog2)
        {
            int miCols = (width + 7) >> 3;
            int sb64Cols = (miCols + 7) >> 3;
            int mn = 0, mx = 1;
            while ((64 << mn) < sb64Cols) mn++;
            while ((sb64Cols >> mx) >= 4) mx++;
            minLog2 = mn;
            maxLog2 = mx - 1;
        }

        private ref struct BitWriter
        {
            private readonly Span<byte> _dst;
            public int Bit;

            public BitWriter(Span<byte> dst)
            {
                _dst = dst;
                Bit = 0;
            }

            public void Put(uint v, int n)
            {
                for (int i = n - 1; i >= 0; i--, Bit++)
                {
                    int b = Bit >> 3;
                    if ((Bit & 7) == 0) _dst[b] = 0;
                    if (((v >> i) & 1) != 0) _dst[b] |= (byte)(0x80 >> (Bit & 7));
                }
            }

            public void PutSigned(int v, int n)
            {
                Put((uint)Math.Abs(v), n);
                Put(v < 0 ? 1u : 0u, 1);
            }
        }

        // Uncompressed header + up to a full compressed header (Vp9CompressedHeader.MaxBytes) before tiles.
        public const int MaxHeaderBytes = 64 + Vp9CompressedHeader.MaxBytes;

        // Writes header + 1 stand-in byte + tiles into dst; returns the length. target: -1 on key frames.
        public static int Write(in Vp9NativeInput p, ReadOnlySpan<int> refIdx, int target, ReadOnlySpan<byte> tiles, Span<byte> dst)
        {
            int n = WriteUncompressed(in p, refIdx, target, 1, false, dst);
            dst[n++] = 0;                      // stand-in compressed header
            tiles.CopyTo(dst[n..]);
            n += tiles.Length;
            return n;
        }

        // Writes header + the real compressed header (defaults -> supplied prob deltas) + tiles; the caller
        // must have cleared the job through Vp9CompressedHeader.Check. Returns the length.
        public static int Write(
            in Vp9NativeInput p, ReadOnlySpan<int> refIdx, int target,
            ReadOnlySpan<byte> probs, ReadOnlySpan<byte> defaults, ReadOnlySpan<byte> tiles, Span<byte> dst)
        {
            Span<byte> compressed = stackalloc byte[Vp9CompressedHeader.MaxBytes];
            int nCompressed = Vp9CompressedHeader.Write(in p, probs, defaults, compressed);

            int n = WriteUncompressed(in p, refIdx, target, nCompressed, true, dst);
            compressed[..nCompressed].CopyTo(dst[n..]);
            tiles.CopyTo(dst[(n + nCompressed)..]);
            n += nCompressed + tiles.Length;
            if ((dst[n - 1] & 0xe0) == 0xc0)
                dst[n++] = 0;                  // a packet ending in a marker byte would be split as a superframe
            return n;
        }

        // mc: emit the byte-level fixups a real decoder requires (interp literal remap, tail-bit mask,
        // marker-pattern pad byte). The libvpx backend consumed the pre-fixup byte shape since this file
        // existed — its Write must keep producing those exact bytes (mc=false).
        private static int WriteUncompressed(in Vp9NativeInput p, ReadOnlySpan<int> refIdx, int target, int headerSize, bool mc, Span<byte> dst)
        {
            bool key = p.KeyFrame;
            bool intraOnly = !key && p.IntraOnly;
            var b = new BitWriter(dst);

            b.Put(2, 2);                       // frame_marker
            b.Put(0, 2);                       // profile 0
            b.Put(0, 1);                       // show_existing_frame
            b.Put(key ? 0u : 1u, 1);           // frame_type
            b.Put(intraOnly ? 0u : 1u, 1);     // show_frame
            b.Put(0, 1);                       // error_resilient_mode

            if (key)
            {
                b.Put(0x49, 8); b.Put(0x83, 8); b.Put(0x42, 8);
                b.Put(0, 3);                   // color_space
                b.Put(0, 1);                   // color_range
                b.Put((uint)(p.Width - 1), 16); b.Put((uint)(p.Height - 1), 16);
                b.Put(0, 1);                   // render_and_frame_size_different
            }
            else
            {
                if (intraOnly) b.Put(1, 1);    // intra_only
                b.Put(0, 2);                   // reset_frame_context

                if (intraOnly)
                {
                    b.Put(0x49, 8); b.Put(0x83, 8); b.Put(0x42, 8);
                    b.Put(1u << target, 8);
                    b.Put((uint)(p.Width - 1), 16); b.Put((uint)(p.Height - 1), 16);
                    b.Put(0, 1);
                }
                else
                {
                    b.Put(1u << target, 8);
                    b.Put((uint)refIdx[0], 3); b.Put((uint)p.SignBias1, 1);
                    b.Put((uint)refIdx[1], 3); b.Put((uint)p.SignBias2, 1);
                    b.Put((uint)refIdx[2], 3); b.Put((uint)p.SignBias3, 1);
                    b.Put(0, 3);               // found_ref x3
                    b.Put((uint)(p.Width - 1), 16); b.Put((uint)(p.Height - 1), 16);
                    b.Put(0, 1);
                    b.Put(p.AllowHighPrecisionMv ? 1u : 0u, 1);
                    b.Put(p.InterpFilter == 4 ? 1u : 0u, 1);
                    if (p.InterpFilter != 4) b.Put((uint)p.InterpFilter, 2);
                }
            }

            b.Put(0, 1);                       // refresh_frame_context
            b.Put(0, 1);                       // frame_parallel_decoding_mode
            b.Put(0, 2);                       // frame_context_idx
            b.Put((uint)p.FilterLevel, 6);
            b.Put((uint)p.Sharpness, 3);
            b.Put(p.ModeRefDeltaEnabled ? 1u : 0u, 1);

            if (p.ModeRefDeltaEnabled)
            {
                b.Put(1, 1);                   // mode_ref_delta_update
                b.Put(1, 1); b.PutSigned(p.RefDelta0, 6);
                b.Put(1, 1); b.PutSigned(p.RefDelta1, 6);
                b.Put(1, 1); b.PutSigned(p.RefDelta2, 6);
                b.Put(1, 1); b.PutSigned(p.RefDelta3, 6);
                b.Put(1, 1); b.PutSigned(p.ModeDelta0, 6);
                b.Put(1, 1); b.PutSigned(p.ModeDelta1, 6);
            }

            b.Put((uint)p.BaseQIndex, 8);
            ReadOnlySpan<int> dq = [p.YDcDeltaQ, p.UvDcDeltaQ, p.UvAcDeltaQ];
            foreach (int d in dq)
            {
                b.Put(d != 0 ? 1u : 0u, 1);
                if (d != 0) b.PutSigned(d, 4);
            }

            b.Put(0, 1);                       // segmentation_enabled
            TileRange(p.Width, out int min, out int max);
            for (int k = min; k < max; k++)
            {
                b.Put(p.Log2TileCols > k ? 1u : 0u, 1);
                if (p.Log2TileCols <= k) break;
            }

            b.Put(p.Log2TileRows != 0 ? 1u : 0u, 1);
            if (p.Log2TileRows != 0) b.Put(p.Log2TileRows == 2 ? 1u : 0u, 1);
            b.Put((uint)headerSize, 16);       // header_size_in_bytes

            int n = (b.Bit + 7) >> 3;
            if (mc && (b.Bit & 7) != 0) dst[n - 1] &= (byte)(0xff << (8 - (b.Bit & 7)));
            return n;
        }
    }
}
