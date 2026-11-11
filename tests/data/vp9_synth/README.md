# 合成 VP9 测试码流

由 `make.sh` 从程序生成的图案编码（ffmpeg `testsrc2` / `mandelbrot`、`resize_enc.c` 里的条纹），**不含任何游戏数据**。
`make.sh` 固定单线程，重跑逐字节一致（ffmpeg 9.0.2 + libvpx-vp9；`resize.ivf` 用 libvpx 1.17.0 编码器）。
用于原生 VP9（`docs/设计_原生VP9/design_phase2_B.md`）的对拍与恒等测试。

| 文件 | 尺寸 | 帧（关键 / 隐藏 / 其余） | 用途 |
|---|---|---|---|
| `adapt_720.ivf` | 1280x720 | 3 / 2 / 45 | 4 列 tile、后向自适应（frame-parallel 0）、隐藏 alt-ref、alt-ref 的参考符号偏置 (0,0,1)（可走复合预测） |
| `fp_ragged.ivf` | 350x198 | 3 / 4 / 45 | 宽高不是 8 的倍数、frame-parallel 1（P4G 的设置）、隐藏 alt-ref |
| `resize.ivf` | 640x360 → 480x272 | 1 / 0 / 31 | 第 16 帧起改尺寸、不插关键帧（缩放参考帧） |
| `interp_lit0.ivf` | 320x240 | 1 / 0 / 15 | 每帧声明 interp 字面量 0（SMOOTH） |
| `interp_lit1.ivf` | 320x240 | 1 / 0 / 15 | 每帧声明 interp 字面量 1（EIGHTTAP） |

`interp_lit*.ivf` 由打过补丁的 libvpx 1.17.0 编码器生成（编码期强制 `cm->interp_filter`，stock ffmpeg/`make.sh` 出不来，来源记录见 `docs/坑记录.md` 插值字面量条目）；其余帧面参数与 libvpx-vp9 默认一致。用于 `tests/vp9_interp_literal_test.sh`：回归「写侧把字面量当枚举二重交换」的像素失配。
