using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;

namespace Ryujinx.Graphics.Shader.Translation.Optimizations
{
    class BindlessElimination
    {
        public static void RunPass(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor)
        {
            // We can turn a bindless into regular access by recognizing the pattern
            // produced by the compiler for separate texture and sampler.
            // We check for the following conditions:
            // - The handle is a constant buffer value.
            // - The handle is the result of a bitwise OR logical operation.
            // - Both sources of the OR operation comes from a constant buffer.
            LinkedListNode<INode> nextNode;

            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = nextNode)
            {
                nextNode = node.Next;

                if (node.Value is not TextureOperation texOp)
                {
                    continue;
                }

                if ((texOp.Flags & TextureFlags.Bindless) == 0)
                {
                    continue;
                }

                if (!TryConvertBindless(block, resourceManager, gpuAccessor, texOp) &&
                    !GenerateBindlessAccess(block, resourceManager, gpuAccessor, texOp, node) &&
                    !TrySplitPhiHandle(block, resourceManager, gpuAccessor, texOp, node))
                {
                    // If we can't do bindless elimination, remove the texture operation.
                    // Set any destination variables to zero.

                    string typeName = texOp.Inst.IsImage()
                        ? texOp.Type.GetGlslImageTypeName(texOp.Format.GetComponentType())
                        : texOp.Type.GlslTextureTypeName;

                    gpuAccessor.Log($"Failed to find handle source for bindless access of type \"{typeName}\".");

                    for (int destIndex = 0; destIndex < texOp.DestsCount; destIndex++)
                    {
                        block.Operations.AddBefore(node, new Operation(Instruction.Copy, texOp.GetDest(destIndex), OperandHelper.Const(0)));
                    }

                    Utils.DeleteNode(node, texOp);
                }
            }
        }

        // S2A（#154，移植自 ha7rock/ff-mobile 3cc9c0c，MIT）：句柄经「各路取值不同」的 phi 到达贴图指令。NVN 编译器把只差句柄的贴图读并成一条，
        //   句柄寄存器在各路各自赋值：Unreal Niagara 的顶点工厂 / 模拟着色器从浮点或半精度数据缓冲读粒子属性，一路是 c2[浮点缓冲]（| 采样器），
        //   另一路是 c2[半精度缓冲] | 采样器，不走到读的路上寄存器里是先前的无关值（地址、浮点数）。TryGetOperation 只在 phi 各入值相同时才看穿，
        //   GenerateBindlessAccess 要求每个入值都来自常量缓冲 / 输入 / 存储缓冲，于是这条读被删、结果置 0：最终幻想 共鸣 3212 支着色器丢读
        //   （顶点 3036、计算 134、片元 42，几乎全是 textureBuffer），Niagara 网格粒子缩放读成 0、缩成一点，招式特效不出。
        //   这里列出 phi 网能给出的每个值（按位运算里的 phi 展开：c2[a] 或 c2[b]，| 采样器），只由 c2（驱动的句柄表）字、常数和按位运算组成且指向
        //   贴图的，在本块重建（常量缓冲字哪里都能读），每个句柄带自己的绑定各读一次，运行期句柄与各重建值比较选结果（都不等取最后一个）。
        //   其余入值是不读贴图的路上寄存器里的值。数组读不动（拷出来的读少了层坐标）。不调 gpuAccessor.Log：直连现编把任何翻译告警当失败。
        private sealed class HandleExpr
        {
            public Operand Leaf;
            public Instruction Inst;
            public HandleExpr[] Sources;
            public string Key;
        }

        private static List<HandleExpr> HandleAlternatives(Operand operand, int depth, HashSet<PhiNode> phis)
        {
            List<HandleExpr> result = new();

            if (operand == null || depth > 8)
            {
                return result;
            }

            if (operand.Type == OperandType.Constant)
            {
                result.Add(new HandleExpr { Leaf = operand, Key = $"#{operand.Value:X}" });
                return result;
            }

            if (operand.Type == OperandType.ConstantBuffer)
            {
                // Only words of c2, where the NVN driver keeps the texture and sampler handles: on paths that do not read the texture the
                // register may hold a word of the game's own constants (a Niagara data offset), which is no handle
                if (operand.GetCbufSlot() == 2)
                {
                    result.Add(new HandleExpr { Leaf = operand, Key = $"c{operand.GetCbufSlot()}[{operand.GetCbufOffset()}]" });
                }

                return result;
            }

            if (operand.Type != OperandType.LocalVariable)
            {
                return result;
            }

            if (operand.AsgOp is PhiNode phi)
            {
                if (!phis.Add(phi))
                {
                    return result;
                }

                for (int index = 0; index < phi.SourcesCount; index++)
                {
                    foreach (HandleExpr alt in HandleAlternatives(phi.GetSource(index), depth + 1, phis))
                    {
                        if (!result.Exists(r => r.Key == alt.Key) && result.Count < 16)
                        {
                            result.Add(alt);
                        }
                    }
                }

                phis.Remove(phi);

                return result;
            }

            if (operand.AsgOp is not Operation op ||
                op.DestsCount != 1 ||
                (op.Inst != Instruction.BitwiseOr &&
                 op.Inst != Instruction.BitwiseAnd &&
                 op.Inst != Instruction.ShiftLeft &&
                 op.Inst != Instruction.ShiftRightU32 &&
                 op.Inst != Instruction.Copy))
            {
                return result;
            }

            if (op.Inst == Instruction.Copy)
            {
                return HandleAlternatives(op.GetSource(0), depth + 1, phis);
            }

            List<HandleExpr[]> partial = new() { Array.Empty<HandleExpr>() };

            for (int index = 0; index < op.SourcesCount; index++)
            {
                List<HandleExpr> alts = HandleAlternatives(op.GetSource(index), depth + 1, phis);

                if (alts.Count == 0)
                {
                    return result;
                }

                List<HandleExpr[]> next = new();

                foreach (HandleExpr[] p in partial)
                {
                    foreach (HandleExpr alt in alts)
                    {
                        if (next.Count < 16)
                        {
                            next.Add([.. p, alt]);
                        }
                    }
                }

                partial = next;
            }

            foreach (HandleExpr[] srcs in partial)
            {
                result.Add(new HandleExpr { Inst = op.Inst, Sources = srcs, Key = $"{op.Inst}(" + string.Join(",", Array.ConvertAll(srcs, x => x.Key)) + ")" });
            }

            return result;
        }

        private static Operand Materialize(HandleExpr expr, List<Operation> code)
        {
            if (expr.Leaf != null)
            {
                return expr.Leaf;
            }

            Operand[] sources = Array.ConvertAll(expr.Sources, x => Materialize(x, code));
            Operand dest = OperandHelper.Local();
            code.Add(new Operation(expr.Inst, dest, sources));

            return dest;
        }

        // A value of the phi web that is not this read's texture: its texture word is outside c2's texture tables, or the binding it resolved to
        // already exists with another type (the register held the handle of a texture this shader reads elsewhere as an array; such a read
        // does not compile)
        private static bool BindingTypeMatches(ResourceManager resourceManager, TextureOperation read)
        {
            const SamplerType Shape = SamplerType.Mask | SamplerType.Array | SamplerType.Multisample;

            // The texture word must lie in c2's texture tables (0x20 combined, 0x120 images, 0x168 separate textures; samplers start at 0x568):
            // a value such as c2[sampler] | c2[sampler] also parses as a handle
            if (!resourceManager.TryGetCbufSlotAndHandleForTexture(new SetBindingPair(read.Set, read.Binding), out int cbufSlot, out int handle) ||
                (cbufSlot & 0xffff) != 2 ||
                (handle & 0x3fff) * 4 < 0x20 ||
                (handle & 0x3fff) * 4 >= 0x568)
            {
                return false;
            }

            foreach (TextureDescriptor descriptor in resourceManager.GetTextureDescriptors())
            {
                if (descriptor.Set == read.Set && descriptor.Binding == read.Binding)
                {
                    return (descriptor.Type & Shape) == (read.Type & Shape);
                }
            }

            return true;
        }

        private static bool TrySplitPhiHandle(
            BasicBlock block,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TextureOperation texOp,
            LinkedListNode<INode> node)
        {
            // Array reads are left alone: their copies came out with the layer missing from the coordinates (2 shaders of FINAL FANTASY RESONANCE
            // did not compile)
            if ((texOp.Inst != Instruction.TextureSample && !texOp.Inst.IsTextureQuery()) || (texOp.Type & SamplerType.Array) != 0)
            {
                return false;
            }

            Operand handle = texOp.GetSource(0);

            if (handle.AsgOp is not PhiNode)
            {
                return false;
            }

            List<(Operand Value, List<Operation> Code, TextureOperation Read)> reads = new();

            foreach (HandleExpr alt in HandleAlternatives(handle, 0, new HashSet<PhiNode>()))
            {
                if (alt.Leaf != null && alt.Leaf.Type == OperandType.Constant)
                {
                    continue;
                }

                List<Operation> code = new();
                Operand value = Materialize(alt, code);

                Operand[] sources = new Operand[texOp.SourcesCount];

                for (int index = 0; index < sources.Length; index++)
                {
                    sources[index] = texOp.GetSource(index);
                }

                sources[0] = value;

                Operand[] dests = new Operand[texOp.DestsCount];

                for (int index = 0; index < dests.Length; index++)
                {
                    dests[index] = OperandHelper.Local();
                }

                TextureOperation read = new(texOp.Inst, texOp.Type, texOp.Format, texOp.Flags, texOp.Set, texOp.Binding, texOp.Index, dests, sources);

                if (TryConvertBindless(block, resourceManager, gpuAccessor, read) && BindingTypeMatches(resourceManager, read))
                {
                    reads.Add((value, code, read));
                }
                else
                {
                    for (int index = 0; index < read.SourcesCount; index++)
                    {
                        read.SetSource(index, null);
                    }
                }

                if (reads.Count == 8)
                {
                    break;
                }
            }

            if (reads.Count == 0)
            {
                return false;
            }

            foreach (var (_, code, read) in reads)
            {
                foreach (Operation op in code)
                {
                    block.Operations.AddBefore(node, op);
                }

                block.Operations.AddBefore(node, read);
            }

            Operand[] conds = new Operand[reads.Count - 1];

            for (int index = 0; index < conds.Length; index++)
            {
                conds[index] = OperandHelper.Local();
                block.Operations.AddBefore(node, new Operation(Instruction.CompareEqual, conds[index], handle, reads[index].Value));
            }

            for (int destIndex = 0; destIndex < texOp.DestsCount; destIndex++)
            {
                Operand result = reads[^1].Read.GetDest(destIndex);

                for (int index = conds.Length - 1; index >= 0; index--)
                {
                    Operand selected = OperandHelper.Local();
                    block.Operations.AddBefore(node, new Operation(Instruction.ConditionalSelect, selected, conds[index], reads[index].Read.GetDest(destIndex), result));
                    result = selected;
                }

                block.Operations.AddBefore(node, new Operation(Instruction.Copy, texOp.GetDest(destIndex), result));
            }

            Utils.DeleteNode(node, texOp);

            return true;
        }

        private static bool GenerateBindlessAccess(
            BasicBlock block,
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TextureOperation texOp,
            LinkedListNode<INode> node)
        {
            if (!gpuAccessor.QueryHostSupportsSeparateSampler())
            {
                // We depend on combining samplers and textures in the shader being supported for this.

                return false;
            }

            Operand bindlessHandle = texOp.GetSource(0);

            if (bindlessHandle.AsgOp is PhiNode phi)
            {
                for (int srcIndex = 0; srcIndex < phi.SourcesCount; srcIndex++)
                {
                    Operand phiSource = phi.GetSource(srcIndex);

                    if (phiSource.AsgOp is not PhiNode && !IsBindlessAccessAllowed(phiSource) &&
                        !(gpuAccessor.QueryBindlessPhiSeeThrough() && IsGuardedTableHandle(bindlessHandle)))
                    {
                        return false;
                    }
                }
            }
            else if (!IsBindlessAccessAllowed(bindlessHandle))
            {
                return false;
            }

            Operand textureHandle = OperandHelper.Local();
            Operand samplerHandle = OperandHelper.Local();
            Operand textureIndex = OperandHelper.Local();

            block.Operations.AddBefore(node, new Operation(Instruction.BitwiseAnd, textureHandle, bindlessHandle, OperandHelper.Const(0xfffff)));
            block.Operations.AddBefore(node, new Operation(Instruction.ShiftRightU32, samplerHandle, bindlessHandle, OperandHelper.Const(20)));

            // S2A（直连）：采样贴图（非贴图缓冲）按整个池长、宿主按编号建池表；贴图缓冲与图像按槽绑，数组长 QueryBindlessSlotArrayLength（0 = 也按池长，上游原样）。
            //   上游参考实现（MIT）BindlessElimination pooled / NvnSlotArrayLength 同此分法
            int slotArrayLength = gpuAccessor.QueryBindlessSlotArrayLength();
            bool pooled = slotArrayLength <= 0 || (!texOp.Inst.IsImage() && (texOp.Type & SamplerType.Mask) != SamplerType.TextureBuffer);

            int texturePoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QueryTextureArrayLengthFromPool()) : slotArrayLength;

            block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, textureIndex, textureHandle, OperandHelper.Const(texturePoolLength - 1)));

            texOp.SetSource(0, textureIndex);

            bool hasSampler = !texOp.Inst.IsImage();

            SetBindingPair textureSetAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                0,
                TextureHandle.PackOffsets(0, 0, TextureHandleType.Direct),
                texturePoolLength,
                hasSampler);

            if (hasSampler)
            {
                Operand samplerIndex = OperandHelper.Local();

                int samplerPoolLength = pooled ? Math.Max(BindlessToArray.MinimumArrayLength, gpuAccessor.QuerySamplerArrayLengthFromPool()) : slotArrayLength;

                block.Operations.AddBefore(node, new Operation(Instruction.MinimumU32, samplerIndex, samplerHandle, OperandHelper.Const(samplerPoolLength - 1)));

                texOp.InsertSource(1, samplerIndex);

                SetBindingPair samplerSetAndBinding = resourceManager.GetTextureOrImageBinding(
                    texOp.Inst,
                    SamplerType.None,
                    texOp.Format,
                    TextureFlags.None,
                    0,
                    TextureHandle.PackOffsets(0, 0, TextureHandleType.Direct),
                    samplerPoolLength);

                texOp.TurnIntoArray(textureSetAndBinding, samplerSetAndBinding);
            }
            else
            {
                texOp.TurnIntoArray(textureSetAndBinding);
            }

            return true;
        }

        // S2A（直连，#82 八方旅人0 落叶）：守卫下拼成的句柄（@P R = c[2][t] | c[2][s]）与读处的守卫对不上时，寄存器在读不执行的路径上还装着别的值（坐标、浮点中间量），
        // phi 汇合后既不是同一个句柄也不全来自常量缓冲。只要汇合的值里有 c[2] 分离采样器表（IsSeparateSamplerWord）拼出的句柄，就按运行时句柄走回退数组：
        // 读执行的路径上值必是拼好的句柄，其余输入只出现在读不执行的路径上。上游原样是把读整个删掉、结果当 0（落叶的风摆掩码读成 0，预深度与基础通道位置不一致，深度 EQUAL 落空成黑块）
        private static bool IsGuardedTableHandle(Operand handle)
        {
            List<Operand> leaves = new();
            CollectLeaves(handle, leaves);

            foreach (Operand leaf in leaves)
            {
                if (leaf.AsgOp is not Operation op || op.Inst != Instruction.BitwiseOr)
                {
                    continue;
                }

                List<Operand> words = new();
                CollectLeaves(op.GetSource(0), words);
                CollectLeaves(op.GetSource(1), words);

                if (words.Exists(IsSeparateSamplerWord))
                {
                    return true;
                }
            }

            return false;
        }

        private static bool IsBindlessAccessAllowed(Operand bindlessHandle)
        {
            if (bindlessHandle.Type == OperandType.ConstantBuffer)
            {
                // Bindless access with handles from constant buffer is allowed.

                return true;
            }

            if (bindlessHandle.AsgOp is not Operation handleOp ||
                handleOp.Inst != Instruction.Load ||
                (handleOp.StorageKind != StorageKind.Input && handleOp.StorageKind != StorageKind.StorageBuffer))
            {
                // Right now, we only allow bindless access when the handle comes from a shader input or storage buffer.
                // This is an artificial limitation to prevent it from being used in cases where it
                // would have a large performance impact of loading all textures in the pool.
                // It might be removed in the future, if we can mitigate the performance impact.

                return false;
            }

            return true;
        }

        private static bool TryConvertBindless(BasicBlock block, ResourceManager resourceManager, IGpuAccessor gpuAccessor, TextureOperation texOp)
        {
            if (texOp.Inst == Instruction.TextureSample || texOp.Inst.IsTextureQuery())
            {
                Operand bindlessHandle = texOp.GetSource(0);

                // In some cases the compiler uses a shuffle operation to get the handle,
                // for some textureGrad implementations. In those cases, we can skip the shuffle.
                if (bindlessHandle.AsgOp is Operation shuffleOp && shuffleOp.Inst == Instruction.Shuffle)
                {
                    bindlessHandle = shuffleOp.GetSource(0);
                }

                bindlessHandle = Utils.FindLastOperation(bindlessHandle, block, undefTopOnly: gpuAccessor.QueryBindlessPhiSeeThrough());

                // Some instructions do not encode an accurate sampler type:
                // - Most instructions uses the same type for 1D and Buffer.
                // - Query instructions may not have any type.
                // For those cases, we need to try getting the type from current GPU state,
                // as long bindless elimination is successful and we know where the texture descriptor is located.
                bool rewriteSamplerType =
                    texOp.Type == SamplerType.TextureBuffer ||
                    texOp.Inst == Instruction.TextureQuerySamples ||
                    texOp.Inst == Instruction.TextureQuerySize;

                if (bindlessHandle.Type == OperandType.ConstantBuffer)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        bindlessHandle.GetCbufOffset(),
                        bindlessHandle.GetCbufSlot(),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }

                if (!TryGetOperation(bindlessHandle.AsgOp, gpuAccessor.QueryBindlessPhiSeeThrough(), out Operation handleCombineOp))
                {
                    return false;
                }

                if (handleCombineOp.Inst != Instruction.BitwiseOr)
                {
                    return false;
                }

                Operand src0 = Utils.FindLastOperation(handleCombineOp.GetSource(0), block);
                Operand src1 = Utils.FindLastOperation(handleCombineOp.GetSource(1), block);

                // For cases where we have a constant, ensure that the constant is always
                // the second operand.
                // Since this is a commutative operation, both are fine,
                // and having a "canonical" representation simplifies some checks below.
                if (src0.Type == OperandType.Constant && src1.Type != OperandType.Constant)
                {
                    (src0, src1) = (src1, src0);
                }

                // S2A（直连，上游参考实现（MIT）67f1d0055）：编译器两个字的先后都出（星际火狐 62 支片元着色器采样器字在前，把采样器字当纹理、绑定落到没有的槽）。
                // c[2] 分离采样器表（0x568 起 128 项，nvn_mtl.m C2_SAMP）里的字是采样器，另一个是纹理
                if (gpuAccessor.QueryBindlessPhiSeeThrough() && IsSeparateSamplerWord(src0) && !IsSeparateSamplerWord(src1))
                {
                    (src0, src1) = (src1, src0);
                }

                TextureHandleType handleType = TextureHandleType.SeparateSamplerHandle;

                // Try to match the following patterns:
                // Masked pattern:
                //  - samplerHandle = samplerHandle & 0xFFF00000;
                //  - textureHandle = textureHandle & 0xFFFFF;
                //  - combinedHandle = samplerHandle | textureHandle;
                //  Where samplerHandle and textureHandle comes from a constant buffer.
                // Shifted pattern:
                //  - samplerHandle = samplerId << 20;
                //  - combinedHandle = samplerHandle | textureHandle;
                //  Where samplerId and textureHandle comes from a constant buffer.
                // Constant pattern:
                //  - combinedHandle = samplerHandleConstant | textureHandle;
                //  Where samplerHandleConstant is a constant value, and textureHandle comes from a constant buffer.
                if (src0.AsgOp is Operation src0AsgOp)
                {
                    if (src1.AsgOp is Operation src1AsgOp &&
                        src0AsgOp.Inst == Instruction.BitwiseAnd &&
                        src1AsgOp.Inst == Instruction.BitwiseAnd)
                    {
                        src0 = GetSourceForMaskedHandle(src0AsgOp, 0xFFFFF);
                        src1 = GetSourceForMaskedHandle(src1AsgOp, 0xFFF00000);

                        // The OR operation is commutative, so we can also try to swap the operands to get a match.
                        if (src0 == null || src1 == null)
                        {
                            src0 = GetSourceForMaskedHandle(src1AsgOp, 0xFFFFF);
                            src1 = GetSourceForMaskedHandle(src0AsgOp, 0xFFF00000);
                        }

                        if (src0 == null || src1 == null)
                        {
                            return false;
                        }
                    }
                    else if (src0AsgOp.Inst == Instruction.ShiftLeft)
                    {
                        Operand shift = src0AsgOp.GetSource(1);

                        if (shift.Type == OperandType.Constant && shift.Value == 20)
                        {
                            src0 = src1;
                            src1 = src0AsgOp.GetSource(0);
                            handleType = TextureHandleType.SeparateSamplerId;
                        }
                    }
                }
                else if (src1.AsgOp is Operation src1AsgOp && src1AsgOp.Inst == Instruction.ShiftLeft)
                {
                    Operand shift = src1AsgOp.GetSource(1);

                    if (shift.Type == OperandType.Constant && shift.Value == 20)
                    {
                        src1 = src1AsgOp.GetSource(0);
                        handleType = TextureHandleType.SeparateSamplerId;
                    }
                }
                else if (src1.Type == OperandType.Constant && (src1.Value & 0xfffff) == 0)
                {
                    handleType = TextureHandleType.SeparateConstantSamplerHandle;
                }

                if (src0.Type != OperandType.ConstantBuffer)
                {
                    return false;
                }

                if (handleType == TextureHandleType.SeparateConstantSamplerHandle)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        TextureHandle.PackOffsets(src0.GetCbufOffset(), (src1.Value >> 20) & 0xfff, handleType),
                        TextureHandle.PackSlots(src0.GetCbufSlot(), 0),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }
                else if (src1.Type == OperandType.ConstantBuffer)
                {
                    SetHandle(
                        resourceManager,
                        gpuAccessor,
                        texOp,
                        TextureHandle.PackOffsets(src0.GetCbufOffset(), src1.GetCbufOffset(), handleType),
                        TextureHandle.PackSlots(src0.GetCbufSlot(), src1.GetCbufSlot()),
                        rewriteSamplerType,
                        isImage: false);

                    return true;
                }
            }
            else if (texOp.Inst.IsImage())
            {
                Operand src0 = Utils.FindLastOperation(texOp.GetSource(0), block);

                if (src0.Type == OperandType.ConstantBuffer)
                {
                    int cbufOffset = src0.GetCbufOffset();
                    int cbufSlot = src0.GetCbufSlot();

                    if (texOp.Format == TextureFormat.Unknown)
                    {
                        if (texOp.Inst == Instruction.ImageAtomic)
                        {
                            texOp.Format = ShaderProperties.GetTextureFormatAtomic(gpuAccessor, cbufOffset, cbufSlot);
                        }
                        else
                        {
                            texOp.Format = ShaderProperties.GetTextureFormat(gpuAccessor, cbufOffset, cbufSlot);
                        }
                    }

                    bool rewriteSamplerType = texOp.Type == SamplerType.TextureBuffer;

                    SetHandle(resourceManager, gpuAccessor, texOp, cbufOffset, cbufSlot, rewriteSamplerType, isImage: true);

                    return true;
                }
            }

            return false;
        }

        private static bool TryGetOperation(INode asgOp, bool seeThrough, out Operation outOperation)
        {
            if (asgOp is PhiNode upstreamPhi && !seeThrough)
            {
                // 上游原样（通用模式）：只比 phi 的直接输入
                Operand first = upstreamPhi.GetSource(0);

                for (int index = 1; index < upstreamPhi.SourcesCount; index++)
                {
                    if (!IsSameOperand(first, upstreamPhi.GetSource(index), seeThrough: false))
                    {
                        outOperation = null;

                        return false;
                    }
                }

                asgOp = first.AsgOp;
            }
            else if (asgOp is PhiNode phi)
            {
                // If we have a phi, let's check if all inputs are effectively the same value.
                // If so, we can "see through" the phi and pick any of the inputs (since they are all the same).
                // S2A（直连，原 graphics/nvn/third_party/ryujinx 的改动；IGpuAccessor.QueryBindlessPhiSeeThrough 为真时）：the inputs are gathered through the phis they come from, as a loop makes (a handle kept in a loop variable
                // reaches the phi from the loop's own phi). Upstream compared the phi's direct inputs only and gave up on such handles.

                List<Operand> leaves = new();
                HashSet<PhiNode> visited = new();
                Stack<PhiNode> pending = new();
                pending.Push(phi);

                while (pending.Count > 0)
                {
                    PhiNode current = pending.Pop();

                    if (!visited.Add(current))
                    {
                        continue;
                    }

                    for (int index = 0; index < current.SourcesCount; index++)
                    {
                        Operand src = current.GetSource(index);

                        if (src.AsgOp is PhiNode srcPhi)
                        {
                            pending.Push(srcPhi);
                        }
                        else
                        {
                            leaves.Add(src);
                        }
                    }
                }

                // S2A（上游参考实现（MIT）4a0551e4b 的语义）：守卫下拼成的句柄（R = c[b][a]; @P R |= c[b][s]），读处的守卫又对不上拼的守卫：phi 里还有句柄的第一个字本身和
                // 寄存器原先的值（未定义）。未定义的输入是读不执行的路径；单独一个字、又正是某个 OR 的一个源，是半路的句柄而不是另一个句柄。其余输入照旧须相同
                List<Operation> ors = leaves.ConvertAll(l => l.AsgOp as Operation).FindAll(o => o != null && o.Inst == Instruction.BitwiseOr);

                List<Operand> orWords = new();

                foreach (Operation o in ors)
                {
                    CollectLeaves(o.GetSource(0), orWords);
                    CollectLeaves(o.GetSource(1), orWords);
                }

                leaves.RemoveAll(l => l.Type == OperandType.Undefined ||
                    (l.Type == OperandType.ConstantBuffer && orWords.Exists(w => AreBothEqualConstantBuffers(w, l))));

                Operand firstSrc = leaves.Count != 0 ? leaves[0] : null;

                if (firstSrc == null || !leaves.TrueForAll(l => IsSameOperand(firstSrc, l, seeThrough: true)))
                {
                    outOperation = null;

                    return false;
                }

                asgOp = firstSrc.AsgOp;
            }

            if (asgOp is Operation operation)
            {
                outOperation = operation;

                return true;
            }

            outOperation = null;

            return false;
        }

        private static bool IsSameOperand(Operand x, Operand y, bool seeThrough)
        {
            if (seeThrough && x == y)
            {
                return true;
            }

            if (x.Type == y.Type && x.Type == OperandType.LocalVariable)
            {
                // Reference port: the second source may also be the same constant (a constant sampler ORed into a texture handle).
                return x.AsgOp is Operation xOp &&
                    y.AsgOp is Operation yOp &&
                    xOp.Inst == Instruction.BitwiseOr &&
                    yOp.Inst == Instruction.BitwiseOr &&
                    AreBothEqualConstantBuffers(xOp.GetSource(0), yOp.GetSource(0)) &&
                    (AreBothEqualConstantBuffers(xOp.GetSource(1), yOp.GetSource(1)) || (seeThrough && AreBothEqualConstants(xOp.GetSource(1), yOp.GetSource(1))));
            }

            return false;
        }

        // The values an operand can take through the phis it comes from (each phi visited once)
        private static void CollectLeaves(Operand operand, List<Operand> leaves)
        {
            HashSet<PhiNode> visited = new();
            Stack<Operand> pending = new();
            pending.Push(operand);

            while (pending.Count > 0)
            {
                Operand o = pending.Pop();

                if (o.AsgOp is PhiNode phi)
                {
                    if (visited.Add(phi))
                    {
                        for (int i = 0; i < phi.SourcesCount; i++)
                        {
                            pending.Push(phi.GetSource(i));
                        }
                    }
                }
                else
                {
                    leaves.Add(o);
                }
            }
        }

        private static bool IsSeparateSamplerWord(Operand operand)
        {
            const int Base = 0x568, End = Base + 8 * 128;   // c[2] 分离采样器表（nvn_mtl.m C2_SAMP），8 字节一项

            return operand.Type == OperandType.ConstantBuffer && operand.GetCbufSlot() == 2 && operand.GetCbufOffset() * 4 is >= Base and < End;
        }

        private static bool AreBothEqualConstantBuffers(Operand x, Operand y)
        {
            return x.Type == y.Type && x.Value == y.Value && x.Type == OperandType.ConstantBuffer;
        }

        private static bool AreBothEqualConstants(Operand x, Operand y)
        {
            return x.Type == y.Type && x.Value == y.Value && x.Type == OperandType.Constant;
        }

        private static Operand GetSourceForMaskedHandle(Operation asgOp, uint mask)
        {
            // Assume it was already checked that the operation is bitwise AND.

            Operand src0 = asgOp.GetSource(0);
            Operand src1 = asgOp.GetSource(1);

            if (src0.Type == OperandType.ConstantBuffer && src1.Type == OperandType.ConstantBuffer)
            {
                // We can't check if the mask matches here as both operands are from a constant buffer.
                // Be optimistic and assume it matches. Avoid constant buffer 1 as official drivers
                // uses this one to store compiler constants.

                return src0.GetCbufSlot() == 1 ? src1 : src0;
            }
            else if (src0.Type == OperandType.ConstantBuffer && src1.Type == OperandType.Constant)
            {
                if ((uint)src1.Value == mask)
                {
                    return src0;
                }
            }
            else if (src0.Type == OperandType.Constant && src1.Type == OperandType.ConstantBuffer)
            {
                if ((uint)src0.Value == mask)
                {
                    return src1;
                }
            }

            return null;
        }

        private static void SetHandle(
            ResourceManager resourceManager,
            IGpuAccessor gpuAccessor,
            TextureOperation texOp,
            int cbufOffset,
            int cbufSlot,
            bool rewriteSamplerType,
            bool isImage)
        {
            if (rewriteSamplerType)
            {
                SamplerType newType = gpuAccessor.QuerySamplerType(cbufOffset, cbufSlot);

                if (texOp.Inst.IsTextureQuery())
                {
                    texOp.Type = newType;
                }
                else if (texOp.Type == SamplerType.TextureBuffer && newType == SamplerType.Texture1D)
                {
                    int coordsCount = 2;

                    if (InstEmit.Sample1DAs2D)
                    {
                        newType = SamplerType.Texture2D;
                        texOp.InsertSource(coordsCount++, OperandHelper.Const(0));
                    }

                    if (!isImage &&
                        (texOp.Flags & TextureFlags.IntCoords) != 0 &&
                        (texOp.Flags & TextureFlags.LodLevel) == 0)
                    {
                        // IntCoords textures must always have explicit LOD.
                        texOp.SetLodLevelFlag();
                        texOp.InsertSource(coordsCount, OperandHelper.Const(0));
                    }

                    texOp.Type = newType;
                }
            }

            // S2A：texel buffer 图像操作的声明数值型必须与绑定视图一致（VUID-07753）。无绑定路径拿不到
            // 描述符格式，转有绑定后按描述符补齐——Unknown 时保持原样（与直接路径的未知格式行为一致）。
            if (isImage &&
                (texOp.Type & SamplerType.Mask) == SamplerType.TextureBuffer &&
                texOp.Format == TextureFormat.Unknown)
            {
                texOp.Format = gpuAccessor.QueryTextureFormat(cbufOffset, cbufSlot);
            }

            SetBindingPair setAndBinding = resourceManager.GetTextureOrImageBinding(
                texOp.Inst,
                texOp.Type,
                texOp.Format,
                texOp.Flags & ~TextureFlags.Bindless,
                cbufSlot,
                cbufOffset);

            texOp.SetBinding(setAndBinding);
        }
    }
}
