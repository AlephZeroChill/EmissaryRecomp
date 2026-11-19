using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// MKW_SINGLE_C=0 turns this off. fmuls and the fused single-precision family round their C operand to a 25-bit
    /// significand first (runtime isa/ppc_isa_float.h); that rounding is the identity on a value that is already
    /// single-precision, so where C provably is one the emitter calls the variants without it
    /// (PpcFmulsSingleCInline, PPC_FmaddsSingleC, ...).
    /// </summary>
    private static readonly bool SingleCOperands = Environment.GetEnvironmentVariable("MKW_SINGLE_C") != "0";

    /// <summary>The body being emitted: its FPR SSA values proven single-precision (<see cref="ComputeSingleFprValues"/>).</summary>
    [ThreadStatic]
    private static HashSet<string>? _singleFprValues;

    /// <summary>
    /// The body being emitted stores psq through PPC_PsqStSegmentInline (MKW_LEAN_RANGES), not PPC_PsqStResolvedInline:
    /// off where a state-free clone is derived from the body's text, whose rewrite knows only the resolved spelling.
    /// </summary>
    [ThreadStatic]
    private static bool _segmentPsqStores;

    private static readonly HashSet<string> SinglePrecisionProducers = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_Fadds", "PPC_Fsubs", "PPC_Fmuls", "PPC_Fdivs", "PPC_Fmadds", "PPC_Fmsubs", "PPC_Fnmadds", "PPC_Fnmsubs",
        "PPC_FaddsNoNi", "PPC_FsubsNoNi", "PPC_FmulsNoNi", "PPC_FdivsNoNi", "PPC_Fres"
    };

    private static readonly HashSet<string> FusedSingleTargets = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_Fmadds", "PPC_Fmsubs", "PPC_Fnmadds", "PPC_Fnmsubs"
    };

    /// <summary>
    /// The FPR SSA values that hold a single-precision value on every path: the results of lfs (a 4-byte load into
    /// an FPR: the exact widening of a single), of frsp and of the single-precision arithmetic (each rounds its result
    /// to float), and fmr / fneg / fabs / fsel / phis of such values (a move or a sign change of a single is one).
    /// The greatest fixed point: every member's rule holds with the others members, so cycles through phis qualify
    /// only when every value entering them does.
    /// </summary>
    private static HashSet<string> ComputeSingleFprValues(IrFunction function)
    {
        var unconditional = new HashSet<string>(StringComparer.Ordinal);
        var conditional = new Dictionary<string, string[]>(StringComparer.Ordinal);
        foreach (var block in function.Blocks)
            foreach (var instruction in block.Instructions)
                switch (instruction)
                {
                    case IrCall { Destination: { } dest } call when IsFloatRegister(dest):
                        if (SinglePrecisionProducers.Contains(call.Target))
                            unconditional.Add(dest);
                        else if (call.Target.Equals("PPC_Fsel", StringComparison.OrdinalIgnoreCase) &&
                                 call.Arguments.Count == 3 &&
                                 call.Arguments[1] is { Kind: "register", RegisterName: { } b } &&
                                 call.Arguments[2] is { Kind: "register", RegisterName: { } c })
                            conditional[dest] = new[] { b, c };
                        break;
                    case IrLoad { SizeBytes: 4 } load when IsFloatRegister(load.Destination):
                        unconditional.Add(load.Destination);
                        break;
                    case IrResolvedLoad { SizeBytes: 4 } rload when IsFloatRegister(rload.Destination):
                        unconditional.Add(rload.Destination);
                        break;
                    case IrBinary { Op: "frsp" } frsp when IsFloatRegister(frsp.Destination):
                        unconditional.Add(frsp.Destination);
                        break;
                    case IrBinary { Op: "fneg" or "fabs", Left: { Kind: "register", RegisterName: { } source } } sign
                        when IsFloatRegister(sign.Destination) && IsFloatRegister(source):
                        conditional[sign.Destination] = new[] { source };
                        break;
                    case IrAssign { Value: { Kind: "register", RegisterName: { } source } } move
                        when IsFloatRegister(move.Destination) && IsFloatRegister(source):
                        conditional[move.Destination] = new[] { source };
                        break;
                    case IrPhi phi when IsFloatRegister(phi.Destination):
                        conditional[phi.Destination] = phi.Sources.Values.ToArray();
                        break;
                }

        // A name defined more than once (outside SSA) does not qualify: one definition's rule says nothing of another.
        var definitions = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var block in function.Blocks)
            foreach (var instruction in block.Instructions)
                foreach (var defined in IrRegisterDataFlow.Definitions(instruction))
                    definitions[defined] = definitions.GetValueOrDefault(defined) + 1;
        var singles = new HashSet<string>(unconditional, StringComparer.Ordinal);
        singles.UnionWith(conditional.Keys);
        singles.RemoveWhere(name => definitions.GetValueOrDefault(name) != 1);
        bool changed;
        do
        {
            changed = false;
            foreach (var (name, sources) in conditional)
                if (singles.Contains(name) && !sources.All(singles.Contains))
                {
                    singles.Remove(name);
                    changed = true;
                }
        } while (changed);
        return singles;
    }

    /// <summary>Whether <paramref name="value"/> is an FPR operand holding a single-precision value here.</summary>
    private static bool IsSingleFprOperand(IrValue value, IReadOnlyDictionary<string, bool> localPaired)
    {
        if (!SingleCOperands || value is not { Kind: "register", RegisterName: { } name } || !IsFloatRegister(name))
            return false;
        // A paired register's scalar view is its PS0, a float (ToScalarFloatExpression: PPC_PsToScalarInline).
        if (localPaired.TryGetValue(GetRegisterBaseName(name), out var paired) && paired)
            return true;
        return _singleFprValues?.Contains(name) == true;
    }
}
