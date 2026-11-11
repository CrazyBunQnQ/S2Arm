using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation.Transforms
{
    /// <summary>
    /// S2A（#82 八方旅人 0 地面黑块，11-10）：降精度（QueryHostReducedPrecision）下，写到 gl_Position 的整条依赖链上的浮点运算仍标精确（ForcePrecise → SPIR-V NoContraction）。
    /// 降精度去掉了全部 NoContraction，只靠给 gl_Position 加 invariant 保位置一致；但 Metal 前端按表达式语法决定乘加融合（fp-contract=on：同一个表达式里的 a * b + c 才融合），
    /// SPIRV-Cross 把单次使用的临时量内联进表达式，内联与否取决于着色器里别的输出用不用这些中间量，所以同一段位置运算在深度预渲染（只写位置）和基础遍（还输出光照用的中间量）
    /// 两支顶点着色器里融合得不一样，舍入不同。基础遍按深度「相等」测试，位置差一个末位就整片过不了，GBuffer 留着清屏值：草丛形的黑块（八方旅人 0 实测：同一株草两支着色器
    /// 的位置链 12 个对 20 个 fmuladd，裁剪坐标 y / w 逐位不同）。标精确后每一步单独舍入，同 Maxwell 的 FMUL / FADD 分步舍入，两支着色器同一组指令算出同一个位置。
    /// 只标位置链（其余输出和片元阶段照样降精度）；按 SSA 定义（AsgOp）与 phi 往回走，读回输出位置的 Load 也在链上（它前面写位置的值本身就是起点）
    /// </summary>
    static class S2APositionPrecise
    {
        public static bool IsEnabled(IGpuAccessor gpuAccessor, ShaderStage stage)
        {
            return gpuAccessor.QueryHostReducedPrecision() &&
                   stage is ShaderStage.Vertex or ShaderStage.TessellationControl or ShaderStage.TessellationEvaluation or ShaderStage.Geometry;
        }

        public static void Run(TransformContext context)
        {
            if (!IsEnabled(context.GpuAccessor, context.Stage))
            {
                return;
            }

            Stack<Operand> work = new();

            foreach (BasicBlock block in context.Blocks)
            {
                foreach (INode node in block.Operations)
                {
                    if (node is Operation op &&
                        op.Inst == Instruction.Store &&
                        op.StorageKind.IsOutput() &&
                        op.SourcesCount >= 2 &&
                        op.GetSource(0).Type == OperandType.Constant &&
                        op.GetSource(0).Value == (int)IoVariable.Position)
                    {
                        work.Push(op.GetSource(op.SourcesCount - 1));
                    }
                }
            }

            HashSet<INode> seen = [];

            while (work.TryPop(out Operand value))
            {
                if (value == null || value.Type != OperandType.LocalVariable || value.AsgOp == null || !seen.Add(value.AsgOp))
                {
                    continue;
                }

                INode def = value.AsgOp;

                if (def is Operation op)
                {
                    op.ForcePrecise = true;
                }

                for (int index = 0; index < def.SourcesCount; index++)
                {
                    work.Push(def.GetSource(index));
                }
            }
        }
    }
}
