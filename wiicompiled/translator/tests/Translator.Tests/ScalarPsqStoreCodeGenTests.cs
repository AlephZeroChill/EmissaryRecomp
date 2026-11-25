using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class ScalarPsqStoreCodeGenTests
{
    [Theory]
    [InlineData(false, 0u, false, true)]
    [InlineData(true, 0u, false, false)]
    [InlineData(false, 1u, false, false)]
    [InlineData(false, 0u, true, false)]
    public void DuplicatesOnlyScalarPairStores(bool paired, uint width, bool knownGqr, bool scalarStore)
    {
        var instructions = new List<IrInstruction>
        {
            new IrResolveGuestMemoryRange("range", IrValue.Register("r3"), 0, 16, true)
        };
        if (paired)
            instructions.Add(new IrResolvedPsqLoad("f1", "range", IrValue.Register("r3"), 0, 0, 0, null));
        instructions.Add(new IrResolvedPsqStore("range", IrValue.Register("r3"), 8,
            IrValue.Register("f1"), width, 0, knownGqr ? 0u : null));
        instructions.Add(new IrReturn(IrValue.Register("r3")));
        var function = new IrFunction("scalar_psq_store", "entry",
            new[] { new IrBasicBlock("entry", instructions) });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r3"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64
        });
        var code = new CxxLinearCodeGenerator().Emit(0x80006000,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("scalar_psq_store", ValueRepresentation.UInt32), types);
        Assert.Equal(scalarStore, code.Contains("PPC_PsqStScalarSegmentInline<0u>"));
        if (knownGqr) Assert.Contains("PPC_PsqStKnownResolvedInline<0u, 0u, 0x00000000u>", code);
        else if (!scalarStore) Assert.Contains($"PPC_PsqStSegmentInline<{width}u, 0u>", code);
    }
}
