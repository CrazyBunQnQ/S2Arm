// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
// #82 八方旅人 0 地面黑块：降精度翻译（QueryHostReducedPrecision，直连出库缺省）下，写 gl_Position 的依赖链仍须标精确（ForcePrecise → SPIR-V NoContraction），
//   否则 Metal 前端按表达式语法融合乘加，深度预渲染与基础遍两支顶点着色器的位置舍入不同，基础遍深度「相等」测试整片失败。
// 手搭真 SSA IR（Operation / PhiNode / LocalVariable，与 Ssa.Rename 之后同形），跑生产入口 TransformPasses.RunPass，逐个运算断言 ForcePrecise。
// 期望不跑被测实现：哪些运算在位置链上由下面的构造直接读出（注释里逐个写明），不从结果反推。
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using Ryujinx.Graphics.Shader.Translation.Transforms;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

static class Program
{
    private static int _passed;
    private static int _failed;

    private sealed class Accessor(bool reduced) : IGpuAccessor
    {
        public bool QueryHostReducedPrecision() => reduced;
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => default;
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) => new(0, 0);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, index);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) => new(0, 0);
        public int QuerySamplerArrayLengthFromPool() => 0;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 0;
        public int QueryTextureArrayLengthFromPool() => 0;
    }

    // 一支顶点着色器的 IR 骨架：四个基本块（b0 → b1 / b2 → b3），叶子是常量缓冲读（不是浮点运算，也不是位置输出）
    private sealed class Shader
    {
        public readonly BasicBlock[] Blocks = [new(0), new(1), new(2), new(3)];
        public readonly Dictionary<string, Operation> Ops = [];
        private int _cb;

        public Operand Leaf(int block = 0)
        {
            Operand d = Local();
            Blocks[block].Operations.AddLast(new Operation(Instruction.Load, StorageKind.ConstantBuffer, d, Const(0), Const(0), Const(_cb++), Const(0)));
            return d;
        }

        public Operand Fp(string name, Instruction inst, int block, params Operand[] src)
        {
            Operand d = Local();
            Operation op = new(inst | Instruction.FP32, d, src);
            Blocks[block].Operations.AddLast(op);
            Ops[name] = op;
            return d;
        }

        public void StorePosition(int component, Operand value, int block = 3) =>
            Blocks[block].Operations.AddLast(new Operation(Instruction.Store, StorageKind.Output, null, Const((int)IoVariable.Position), Const(component), value));

        public Operand LoadPosition(int component, int block = 3)
        {
            Operand d = Local();
            Blocks[block].Operations.AddLast(new Operation(Instruction.Load, StorageKind.Output, d, Const((int)IoVariable.Position), Const(component)));
            return d;
        }

        public void StoreUser(int location, Operand value, int block = 3) =>
            Blocks[block].Operations.AddLast(new Operation(Instruction.Store, StorageKind.Output, null, Const((int)IoVariable.UserDefined), Const(location), Const(0), value));
    }

    // 位置链 / 非位置链各运算（期望直接由构造读出）：
    //   pos.x = (a * b) + c                       → mulAB、addX 在链上
    //   pos.y = shared + a，user0.x = shared * a   → shared（b * c）在链上（位置和别的输出共用），userMul 不在
    //   user1 = a * c                             → userOnly 不在
    //   pos.z = phi(b1: a * a, b2: b * b)         → phiL、phiR 在链上（经 phi 往回走）
    //   pos.w 先写 a + b，读回后 fma(读回, c, a) 再写 → firstW、fmaW 在链上（读回的 Load 的前驱就是先写的值本身）
    private static Shader Build()
    {
        Shader s = new();
        Operand a = s.Leaf(), b = s.Leaf(), c = s.Leaf();
        Operand ab = s.Fp("mulAB", Instruction.Multiply, 0, a, b);
        Operand x = s.Fp("addX", Instruction.Add, 0, ab, c);
        Operand sh = s.Fp("shared", Instruction.Multiply, 0, b, c);
        Operand y = s.Fp("addY", Instruction.Add, 0, sh, a);
        Operand um = s.Fp("userMul", Instruction.Multiply, 0, sh, a);
        Operand uo = s.Fp("userOnly", Instruction.Multiply, 0, a, c);
        Operand l = s.Fp("phiL", Instruction.Multiply, 1, a, a);
        Operand r = s.Fp("phiR", Instruction.Multiply, 2, b, b);
        Operand z = Local();
        PhiNode phi = new(z);
        phi.AddSource(s.Blocks[1], l);
        phi.AddSource(s.Blocks[2], r);
        s.Blocks[3].Operations.AddFirst(phi);
        Operand w0 = s.Fp("firstW", Instruction.Add, 3, a, b);
        s.StorePosition(0, x);
        s.StorePosition(1, y);
        s.StorePosition(2, z);
        s.StorePosition(3, w0);
        Operand back = s.LoadPosition(3);
        Operand w1 = s.Fp("fmaW", Instruction.FusedMultiplyAdd, 3, back, c, a);
        s.StorePosition(3, w1);
        s.StoreUser(0, um);
        s.StoreUser(1, uo);
        return s;
    }

    private static readonly string[] OnChain = ["mulAB", "addX", "shared", "addY", "phiL", "phiR", "firstW", "fmaW"];
    private static readonly string[] OffChain = ["userMul", "userOnly"];

    private static void Transform(Shader s, ShaderStage stage, bool reduced)
    {
        FeatureFlags used = FeatureFlags.None;
        TransformContext ctx = new(null, s.Blocks, new ShaderDefinitions(stage, 0, null), null, new Accessor(reduced), TargetApi.Vulkan, TargetLanguage.Spirv, stage, ref used, null);
        TransformPasses.RunPass(ctx);   // 生产入口（S2APositionPrecise 挂在这里）；其余遍对这份 IR 是空操作
    }

    private static void Expect(Shader s, string[] names, bool precise, string what)
    {
        string[] bad = names.Where(n => s.Ops[n].ForcePrecise != precise).ToArray();
        if (bad.Length != 0) throw new Exception($"{what}: {string.Join(", ", bad)} ForcePrecise is not {precise}");
    }

    private static void Run(string name, Action test)
    {
        try { test(); _passed++; Console.WriteLine($"PASS {name}"); }
        catch (Exception error) { _failed++; Console.Error.WriteLine($"FAIL {name}: {error.Message}"); }
    }

    private static int Main()
    {
        foreach (ShaderStage stage in new[] { ShaderStage.Vertex, ShaderStage.TessellationEvaluation, ShaderStage.Geometry })
        {
            Run($"{stage} reduced: position chain precise, other outputs not", () =>
            {
                Shader s = Build();
                Transform(s, stage, true);
                Expect(s, OnChain, true, "position chain");
                Expect(s, OffChain, false, "non-position outputs");
            });
        }

        Run("Fragment reduced: nothing marked (no position output there; ForcePreciseEnable's divide rule does not match)", () =>
        {
            Shader s = Build();
            Transform(s, ShaderStage.Fragment, true);
            Expect(s, OnChain.Concat(OffChain).ToArray(), false, "fragment");
        });

        Run("Vertex not reduced: nothing marked (codegen already emits NoContraction for every operation)", () =>
        {
            Shader s = Build();
            Transform(s, ShaderStage.Vertex, false);
            Expect(s, OnChain.Concat(OffChain).ToArray(), false, "not reduced");
        });

        Console.WriteLine($"position precise: {_passed} passed, {_failed} failed");
        return _failed == 0 ? 0 : 1;
    }
}
