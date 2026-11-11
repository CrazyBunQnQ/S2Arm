#nullable disable
using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Video;
using System;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Nvdec.Android
{
    // AMediaCodec 版的 IVp9NativeBackend（#44）：同一契约（一作业一图、
    // NotRepresentable 作业交给托管解码器直到下个关键帧）下接 s2a_mc shim。
    //
    // MediaCodec 没有 hook 的带外注入通道：tx_mode / reference_mode / 逐作业概率表
    // 全部靠重打包帧自带的真压缩头写进硬件（Vp9CompressedHeader——默认表→供给表的
    // 规范增量；写不出的段落在上游 Vp9CompressedHeader.Check 已拦成托管作业）。
    // use_prev_frame_mvs 不在压缩头里：解码器按未压缩状态推导，整段原生段内推导值
    // 与作业标志同源一致。
    //
    // 这里剩下的判门只有运行时事实，如实回退不伪造：
    //   · ShowFrame=0（intra-only 隐藏帧）：隐藏帧的像素只进参考槽不出图，MediaCodec 拿不到——
    //     hook 经 frame_to_show 交得出，这里交不出，回 NotRepresentable。
    //   · 尺寸变化的非关键帧：codec 重建丢参考状态，回 NeedsKeyFrame。
    //   · mvsOut / countsOut 是硬件产不出的量：一概不写（客体缓冲保持上游真值；
    //     路径的 _counts 维持全零，恰是重打包帧 refresh_frame_context=0 的忠实值——
    //     不产生反向更新），不写「全零」这种具体假值。
    //
    // 管线延迟按「已喂 shown AU − 已出图」记账（_pending）：本作业出图即 Decoded，
    // 有界预算补不齐就 NotRepresentable——这是同步契约下唯一诚实的映射；
    // 连续补齐失败升级 Error 计入路径的禁用阈值。
    //
    // 结局记账（真机验证用）：每 300 作业与 Dispose 各打一行各结局分布。
    internal sealed class McVp9Backend : IVp9NativeBackend
    {
        internal const int ProbsSize = 3087;    // Vp9EntropyProbs
        internal const int CountsSize = 13208;  // Vp9BackwardUpdates
        internal const int MvRefSize = 16;      // Vp9MvRef
        private const int MaxDimension = 8192;  // 与 Vp9NativeHeader.Check 同界
        private const int WaitSliceUs = 10_000; // 每次排空轮询切片
        private const int WaitTotalUs = 60_000; // 一个作业内补齐管线欠账的总预算
        private const int MaxTimeouts = 3;      // 连续欠账升级 Error（死 codec 不空转）

        private McVp9Decoder _decoder;
        private McSurface _surface;
        private int _width, _height;   // codec 当前配置的编码尺寸
        private int _pending;          // 已喂的 show_frame=1 AU 与已收输出的差额
        private int _timeouts;         // 连续预算内没补齐的次数
        private bool _failed;

        // 结局记账（真机覆盖率验证）：每 300 作业与 Dispose 各打一行分布。
        // hidden/shape/starved/error 是本 backend 判出的回退；Vp9NativePath 层面拦下的作业
        // （上游 Check 的帧形段、libvpx 结果、尺寸/关键帧等）经 NoteFallback 记进 fb: 账本。
        private int _jobs, _decoded, _nrHidden, _nrShape, _nrSize, _starved, _errors;

        // Vp9NativePath 交回托管的作业按原因计数，随 outcome 行同排输出，分母是总作业数。
        private readonly SortedDictionary<string, int> _fallbacks = new();

        public void NoteFallback(string reason)
        {
            _fallbacks.TryGetValue(reason, out int n);
            _fallbacks[reason] = n + 1;
        }

        private string FallbackLedger()
        {
            if (_fallbacks.Count == 0) return "";
            var b = new System.Text.StringBuilder(" fb:");
            foreach (var kv in _fallbacks) b.Append($" {kv.Key}={kv.Value}/{_jobs}");
            return b.ToString();
        }

        private Vp9NativeResult Done(Vp9NativeResult r, ref int bucket)
        {
            bucket++;
            if (++_jobs % 300 == 0) Report();
            return r;
        }

        private void Report() => Logger.Info?.Print(LogClass.Nvdec,
            $"mc-vp9 outcome: jobs={_jobs} decoded={_decoded} hidden={_nrHidden} shape={_nrShape} size/key={_nrSize} starved={_starved} error={_errors}{FallbackLedger()}");

        // AMediaCodec 交不出 MV 字段 / 反向更新计数：Decoded 不写 mvsOut/countsOut，
        // 客体 MV 缓冲保持上游内容；调用方据此拦下会读陈旧 MV 的托管作业。
        public bool ProducesGuestMetadata => false;

        // hook 不存在的带外注入通道：概率增量只能走位流——真压缩头必须随帧带上。
        public bool NeedsCompressedHeader => true;

        public static McVp9Backend TryCreate() => McVp9Decoder.Available() ? new McVp9Backend() : null;

        public Vp9NativeResult Decode(
            ReadOnlySpan<byte> frame,
            ReadOnlySpan<byte> probs,
            in Vp9NativeParams p,
            ReadOnlySpan<byte> prevMvs,
            Span<byte> mvsOut,
            Span<byte> countsOut,
            out Vp9NativePicture picture,
            out string error)
        {
            picture = default;
            error = null;
            if (_failed)
            {
                error = "mediacodec sticky failure";
                return Done(Vp9NativeResult.Error, ref _errors);
            }
            // 隐藏帧的像素 MediaCodec 拿不到——如实回退，本作业走托管。
            if (p.ShowFrame == 0)
            {
                return Done(Vp9NativeResult.NotRepresentable, ref _nrHidden);
            }
            if (p.Width < 1 || p.Height < 1 || p.Width > MaxDimension || p.Height > MaxDimension ||
                probs.Length != ProbsSize || countsOut.Length != CountsSize ||
                mvsOut.Length == 0 || mvsOut.Length % MvRefSize != 0)
            {
                return Done(Vp9NativeResult.NotRepresentable, ref _nrShape);
            }
            // 尺寸变化：codec 重建丢参考状态，非关键帧如实回 NeedsKeyFrame。
            if (_decoder != null && (p.Width != _width || p.Height != _height))
            {
                _decoder.Dispose();
                _decoder = null;
                _pending = 0;
                _surface?.Dispose();
                _surface = null;
                if (p.IntraFrame == 0)
                {
                    return Done(Vp9NativeResult.NeedsKeyFrame, ref _nrSize);
                }
            }
            _surface ??= new McSurface(p.Width, p.Height);
            _decoder ??= new McVp9Decoder();

            // 进 AU 记账，再按「欠几张补几张」有界排空——VP9 无重排，
            // 第 k 张出图对应第 k 个 shown AU，补到 pending=0 时最后一张就是本作业的。
            _pending++;
            int rc = _decoder.Decode(frame, _surface, out McVp9Decoder.Diagnostic diagnostic);
            if (rc != 0)
            {
                return Sticky("decode", in diagnostic, out error);
            }
            if (diagnostic.Output != 0)
            {
                _pending--;
            }
            for (int waited = 0; _pending > 0 && waited < WaitTotalUs;)
            {
                rc = _decoder.Wait(WaitSliceUs, _surface, out diagnostic);
                if (rc != 0)
                {
                    return Sticky("wait", in diagnostic, out error);
                }
                if (diagnostic.Output != 0)
                {
                    _pending--;
                }
                else
                {
                    waited += WaitSliceUs;
                }
            }
            if (_pending > 0)
            {
                _timeouts++;
                if (_timeouts >= MaxTimeouts)
                {
                    _failed = true;
                    error = $"mediacodec output starved {_timeouts} jobs in a row ({McVp9Decoder.Diag(in diagnostic)})";
                    return Done(Vp9NativeResult.Error, ref _errors);
                }
                return Done(Vp9NativeResult.NotRepresentable, ref _starved);
            }
            _timeouts = 0;
            // mvsOut / countsOut 是硬件产不出的量：不写（客体缓冲保持上游真值），不伪造。
            _width = p.Width;
            _height = p.Height;
            picture = new Vp9NativePicture(
                _surface.YPlane.Pointer, _surface.UPlane.Pointer, _surface.VPlane.Pointer,
                _surface.Stride, _surface.UvStride, p.Width, p.Height);
            return Done(Vp9NativeResult.Decoded, ref _decoded);
        }

        private Vp9NativeResult Sticky(string stage, in McVp9Decoder.Diagnostic diagnostic, out string error)
        {
            _failed = true;
            error = $"mediacodec {stage} {McVp9Decoder.Diag(in diagnostic)}";
            return Done(Vp9NativeResult.Error, ref _errors);
        }

        public void Dispose()
        {
            Report();
            _decoder?.Dispose();
            _decoder = null;
            _surface?.Dispose();
            _surface = null;
        }
    }
}
