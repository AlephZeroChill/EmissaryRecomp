using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Disassembly;

internal static class JumpTableDetector
{
    private const int MaxBacktrackInstructions = 8192;  // SPM: loop-hoisted table bases (lis far above the switch)
    private const int MaxEntryCount = 512;
    private const int MaxTracedPredecessors = 512;  // followControlFlow: a loop's back edge can lead the trace in circles

    public static bool TryRecognize(
        IReadOnlyList<PpcInstruction> ordered,
        IReadOnlyDictionary<uint, int> indexByAddress,
        uint bctrAddress,
        uint functionStart,
        uint functionEnd,
        ProgramImage image,
        out IReadOnlyList<uint> targets,
        bool followControlFlow = false)
    {
        targets = Array.Empty<uint>();
        if (!indexByAddress.TryGetValue(bctrAddress, out var bctrIndex))
        {
            return false;
        }

        var mtctrIndex = FindPrevious(ordered, bctrIndex, "mtctr", limit: 6);
        if (mtctrIndex < 0)
        {
            return false;
        }

        if (ordered[mtctrIndex].Operands.Count == 0 || ordered[mtctrIndex].Operands[0] is not PpcRegisterOperand ctrOperand)
        {
            return false;
        }
        var targetRegister = ctrOperand.Name;

        var loadIndex = FindPreviousLoad(ordered, mtctrIndex, targetRegister);
        if (loadIndex < 0)
        {
            return false;
        }

        var load = ordered[loadIndex];
        if (!string.Equals(load.Mnemonic, "lwzx", StringComparison.OrdinalIgnoreCase))
        {
            return false; // Only handle lwzx-based tables for now.
        }

        if (load.Operands.Count < 3 || load.Operands[0] is not PpcRegisterOperand dest ||
            load.Operands[1] is not PpcRegisterOperand baseReg ||
            load.Operands[2] is not PpcRegisterOperand indexReg)
        {
            return false;
        }

        if (!string.Equals(dest.Name, targetRegister, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var entriesAreRelative = false;
        if (!TryResolveLisAddiConstant(ordered, loadIndex, baseReg.Name, out var tableBase) &&
            !TryResolvePcRelativeLoadedConstant(ordered, loadIndex, baseReg.Name, image, out tableBase))
        {
            return false;
        }
        entriesAreRelative = HasRelativeEntryAdd(ordered, loadIndex, mtctrIndex, targetRegister, baseReg.Name);

        // Code variants (followControlFlow): the bound compare on the index register, first in address order, then along
        // the control flow (through a mod's hook body); no bound, no table, and the variant stays interpreted. Never the
        // any-register fallback: when Project+ puts its hook branch on a switch's bounds compare (the hook adds cases),
        // the address-order trace rightly stops at an older write of the index register, and the fallback took an
        // unrelated compare above it (0x80917190: "cmpwi r0, 0" for the hooked "cmplwi r4, 50", one case). The cases
        // past such a bound left the variant through MKW_INDIRECT_JUMP into its own middle, in the interpreter.
        // Base translation: address order with the fallback (a loop's counter register), as before.
        static bool Valid(bool found, int bound) => found && bound >= 0 && bound < MaxEntryCount;
        string via;
        int upperBound;
        if (!followControlFlow)
        {
            via = "address order";
            if (!Valid(TryFindUpperBound(ordered, loadIndex, indexReg.Name, false, false, out upperBound), upperBound))
            {
                return false;
            }
        }
        else if (Valid(TryFindUpperBound(ordered, loadIndex, indexReg.Name, false, true, out upperBound), upperBound))
        {
            via = "address order";
        }
        else if (Valid(TryFindUpperBound(ordered, loadIndex, indexReg.Name, true, true, out upperBound), upperBound))
        {
            via = "control flow";
        }
        else
        {
            LogTable($"variant fn {functionStart:X8} bctr {bctrAddress:X8} table {tableBase:X8} no bound: not recognized");
            return false;
        }

        var entryCount = upperBound + 1;
        var dedupList = new List<uint>();
        var seen = new HashSet<uint>();
        for (var i = 0; i < entryCount; i++)
        {
            var entryAddress = unchecked(tableBase + (uint)(i * 4));
            if (!TryReadWord(image, entryAddress, out var entry))
            {
                return false;
            }
            var target = entriesAreRelative ? unchecked(tableBase + entry) : entry;

            if (!indexByAddress.ContainsKey(target))
            {
                // Allow targets that haven't been decoded yet as long as they
                // fall within the current function's decoding window.
                if (target < functionStart || target >= functionEnd || (target & 3) != 0)
                {
                    return false;
                }
            }

            if (seen.Add(target))
            {
                dedupList.Add(target);
            }
        }

        if (dedupList.Count == 0)
        {
            return false; // Not a real switch.
        }

        if (LogWriter is not null)
        {
            // run: consecutive table words from the base that would pass the same entry check, an upper estimate of
            // the table's length (a neighbouring table or code-like data extends it) to compare the bound against.
            var run = 0;
            while (run < MaxEntryCount && TryReadWord(image, unchecked(tableBase + (uint)(run * 4)), out var word))
            {
                var target = entriesAreRelative ? unchecked(tableBase + word) : word;
                if (!indexByAddress.ContainsKey(target) && (target < functionStart || target >= functionEnd || (target & 3) != 0))
                {
                    break;
                }
                run++;
            }
            LogTable($"{(followControlFlow ? "variant" : "base")} fn {functionStart:X8} bctr {bctrAddress:X8} table {tableBase:X8} " +
                     $"entries {entryCount} distinct {dedupList.Count} via {via} run {run}");
        }

        targets = dedupList;
        return true;
    }

    // TRANSLATOR_JUMP_TABLE_LOG=<file>: one line per recognized table (and per code-variant switch left unrecognized),
    // for checking recognized sizes against the tables. Off by default; no effect on the translation.
    private static readonly System.IO.StreamWriter? LogWriter =
        Environment.GetEnvironmentVariable("TRANSLATOR_JUMP_TABLE_LOG") is { Length: > 0 } logPath
            ? new System.IO.StreamWriter(logPath, append: false) { AutoFlush = true }
            : null;

    private static void LogTable(string line)
    {
        if (LogWriter is null) return;
        lock (LogWriter) LogWriter.WriteLine(line);
    }

    private static int FindPrevious(IReadOnlyList<PpcInstruction> ordered, int startIndex, string mnemonic, int limit)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= limit; i--)
        {
            // Comparison already ignores case, so lowering the needle only
            // allocated a copy of it.
            if (string.Equals(ordered[i].Mnemonic, mnemonic, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }
        return -1;
    }

    private static bool HasRelativeEntryAdd(
        IReadOnlyList<PpcInstruction> ordered,
        int loadIndex,
        int mtctrIndex,
        string targetRegister,
        string tableBaseRegister)
    {
        for (var i = loadIndex + 1; i < mtctrIndex; i++)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 3 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcRegisterOperand left ||
                ins.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, targetRegister))
            {
                continue;
            }

            if ((RegistersEqual(left.Name, targetRegister) && RegistersEqual(right.Name, tableBaseRegister)) ||
                (RegistersEqual(right.Name, targetRegister) && RegistersEqual(left.Name, tableBaseRegister)))
            {
                return true;
            }
        }

        return false;
    }

    private static int FindPreviousLoad(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand dest)
            {
                continue;
            }

            if (!string.Equals(dest.Name, register, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            // Decoder mnemonics are lowercase by construction.
            if (ins.Mnemonic is "lwzx" or "lwz")
            {
                return i;
            }
        }

        return -1;
    }

    /// <summary>
    /// <c>rlwinm rA, rS, 2, 0, 29</c> (slwi by two) or <c>mulli rA, rS, 4</c>: the case index scaled to a
    /// word offset. Returns the index register <c>rS</c>.
    /// </summary>
    private static bool TryGetScaleByFourSource(PpcInstruction ins, out string source)
    {
        source = string.Empty;
        if (ins.Operands.Count >= 5 && ins.Mnemonic is "rlwinm" &&
            ins.Operands[1] is PpcRegisterOperand rs &&
            ins.Operands[2] is PpcImmediateOperand { Value: 2 } &&
            ins.Operands[3] is PpcImmediateOperand { Value: 0 } &&
            ins.Operands[4] is PpcImmediateOperand { Value: 29 })
        {
            source = rs.Name;
            return true;
        }

        if (ins.Operands.Count >= 3 && ins.Mnemonic is "mulli" &&
            ins.Operands[1] is PpcRegisterOperand ra &&
            ins.Operands[2] is PpcImmediateOperand { Value: 4 })
        {
            source = ra.Name;
            return true;
        }

        return false;
    }

    /// <summary>
    /// Whether an instruction whose first operand is a GPR writes it. Stores, compares, traps and
    /// moves to special registers only read their first operand.
    /// </summary>
    private static bool WritesFirstOperand(PpcInstruction ins)
    {
        var mnemonic = ins.Mnemonic;
        return !(mnemonic.StartsWith("st", StringComparison.OrdinalIgnoreCase) ||
                 mnemonic.StartsWith("cmp", StringComparison.OrdinalIgnoreCase) ||
                 mnemonic.StartsWith("tw", StringComparison.OrdinalIgnoreCase) ||
                 mnemonic.StartsWith("mt", StringComparison.OrdinalIgnoreCase) ||
                 mnemonic.StartsWith("dcb", StringComparison.OrdinalIgnoreCase) ||
                 mnemonic.StartsWith("icb", StringComparison.OrdinalIgnoreCase));
    }

    private static bool TryResolveLisAddiConstant(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register, out uint value)
    {
        var haveOffset = false;
        int offset = 0;
        var baseRegister = register;
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand dest)
            {
                continue;
            }

            if (!string.Equals(dest.Name, baseRegister, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var mnemonic = ins.Mnemonic;
            if (!haveOffset && mnemonic == "addi" && ins.Operands.Count >= 3 && ins.Operands[1] is PpcRegisterOperand addBase &&
                ins.Operands[2] is PpcImmediateOperand addImm)
            {
                offset = (short)addImm.Value;
                haveOffset = true;
                // Track the source register so we can resolve lis/addi from a different base.
                baseRegister = addBase.Name;
                continue;
            }

            if (mnemonic == "lis" && ins.Operands.Count >= 2 && ins.Operands[1] is PpcImmediateOperand hiImm)
            {
                var hi = hiImm.Value << 16;
                var low = haveOffset ? offset : 0;
                value = unchecked((uint)(hi + low));
                return true;
            }
        }

        value = 0;
        return false;
    }

    private static bool TryResolvePcRelativeLoadedConstant(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 2 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcDisplacementOperand displacement ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            if (!TryResolvePicBaseRegister(ordered, i, displacement.BaseRegister, image, out var picBase))
            {
                continue;
            }

            var literalAddress = AddSigned(picBase, displacement.Offset);
            if (TryReadWord(image, literalAddress, out value))
            {
                return true;
            }
        }

        value = 0;
        return false;
    }

    private static bool TryResolvePicBaseRegister(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var addIndex = startIndex - 1; addIndex >= 0 && startIndex - addIndex <= MaxBacktrackInstructions; addIndex--)
        {
            var add = ordered[addIndex];
            if (!string.Equals(add.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                add.Operands.Count < 3 ||
                add.Operands[0] is not PpcRegisterOperand dest ||
                add.Operands[1] is not PpcRegisterOperand left ||
                add.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            string offsetRegister;
            if (RegistersEqual(left.Name, register))
            {
                offsetRegister = right.Name;
            }
            else if (RegistersEqual(right.Name, register))
            {
                offsetRegister = left.Name;
            }
            else
            {
                continue;
            }

            if (!TryFindRegisterDisplacementLoadBefore(ordered, addIndex, offsetRegister, register, out var offsetLoad) ||
                !TryFindMflrBefore(ordered, offsetLoad.Index, register, out var mflrIndex) ||
                !TryFindLinkCallBefore(ordered, mflrIndex, out var linkAddress))
            {
                continue;
            }

            var offsetAddress = AddSigned(linkAddress, offsetLoad.Displacement);
            if (!TryReadWord(image, offsetAddress, out var picOffset))
            {
                continue;
            }

            value = unchecked(linkAddress + picOffset);
            return true;
        }

        value = 0;
        return false;
    }

    private static bool TryFindRegisterDisplacementLoadBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        string baseRegister,
        out (int Index, int Displacement) result)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 2 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                ins.Operands[1] is PpcDisplacementOperand displacement &&
                RegistersEqual(dest.Name, destinationRegister) &&
                RegistersEqual(displacement.BaseRegister, baseRegister))
            {
                result = (i, displacement.Offset);
                return true;
            }
        }

        result = default;
        return false;
    }

    private static bool TryFindMflrBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        out int mflrIndex)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "mflr", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 1 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                RegistersEqual(dest.Name, destinationRegister))
            {
                mflrIndex = i;
                return true;
            }
        }

        mflrIndex = -1;
        return false;
    }

    private static bool TryFindLinkCallBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        out uint linkAddress)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.IsCall)
            {
                linkAddress = ins.EndAddress;
                return true;
            }
        }

        linkAddress = 0;
        return false;
    }

    private static bool TryFindUpperBound(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register,
        bool followControlFlow, bool codeVariant, out int upperBound)
    {
        // Preferred: the bound compare on the case index that feeds lwzx. Trace the index register
        // backwards through the scale-by-four that turns the case number into a byte offset (CodeWarrior
        // emits "cmplwi rI, N; bgt default; slwi rX, rI, 2; lwzx rT, rBase, rX"), and stop at any other
        // write to the tracked register: a compare found beyond it tests an older, unrelated value (NSMBW's
        // TagProcessor_c::preProcess reuses r0, whose earlier "cmplwi r0, 0" turned a 20-case table into one).
        // followControlFlow (code variants): the trace steps to each instruction's predecessor instead of the word
        // before it, so it can pass through a mod's hook body: Project+ moves 0x8078249C's "cmplwi r4, 76" into a C2
        // body that branches back to the bgt, and the word before that bgt is the hook's branch, which execution
        // never falls through. On straight-line code both traces visit the same instructions.
        var tracked = register;
        var steps = 0;
        for (var i = followControlFlow ? Predecessor(ordered, startIndex) : startIndex - 1;
             i >= 0 && (followControlFlow ? ++steps <= MaxTracedPredecessors : startIndex - i <= MaxBacktrackInstructions);
             i = followControlFlow ? Predecessor(ordered, i) : i - 1)
        {
            var ins = ordered[i];
            // Code variants: the address-order trace also ends where execution cannot fall through into the word after
            // (an unconditional b, blr, bctr): a mod's hook branch there means what lies above it did not run before.
            if (codeVariant && !followControlFlow && !FallsThrough(ins))
            {
                break;
            }
            if (ins.Operands.Count < 2 || ins.Operands[0] is not PpcRegisterOperand reg ||
                !string.Equals(reg.Name, tracked, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (ins.Mnemonic is "cmplwi" or "cmpwi")
            {
                if (ins.Operands[1] is PpcImmediateOperand imm)
                {
                    upperBound = imm.Value;
                    return true;
                }
                continue;
            }

            if (TryGetScaleByFourSource(ins, out var source))
            {
                tracked = source;
                continue;
            }

            if (WritesFirstOperand(ins))
            {
                break;
            }
        }

        // Fallback: some loops compare a logical counter (e.g. r18) distinct from the byte-offset
        // register (e.g. r30) that feeds lwzx, so the bound check may not target the index register.
        // (Not for code variants: they accept only a compare on the index register itself.)
        for (var i = startIndex - 1; !codeVariant && i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count < 2 || ins.Operands[1] is not PpcImmediateOperand imm)
            {
                continue;
            }

            if (ins.Mnemonic is not ("cmplwi" or "cmpwi"))
            {
                continue;
            }

            if (imm.Value < 0 || imm.Value >= MaxEntryCount)
            {
                continue;
            }

            upperBound = imm.Value;
            return true;
        }

        upperBound = 0;
        return false;
    }

    // The instruction executed just before ordered[index]: the word before it when execution falls through from
    // there, else the one branch that reaches it; -1 when there is no such single instruction.
    private static int Predecessor(IReadOnlyList<PpcInstruction> ordered, int index)
    {
        var address = ordered[index].Address;
        if (index > 0 && ordered[index - 1].EndAddress == address && FallsThrough(ordered[index - 1]))
        {
            return index - 1;
        }
        var found = -1;
        for (var i = 0; i < ordered.Count; i++)
        {
            var ins = ordered[i];
            if (ins.IsCall || ins.IsReturn || !ins.BranchTargets.Contains(address))
            {
                continue;
            }
            if (found >= 0)
            {
                return -1;
            }
            found = i;
        }
        return found;
    }

    private static bool FallsThrough(PpcInstruction ins) =>
        ins.IsConditionalBranch || ins.IsConditionalCall ||
        !(ins.IsUnconditionalBranch || ins.IsReturn ||
          string.Equals(ins.Mnemonic, "bctr", StringComparison.OrdinalIgnoreCase));

    private static bool TryReadWord(ProgramImage image, uint address, out uint value)
    {
        if (!image.Contains(address, sizeof(uint)))
        {
            value = 0;
            return false;
        }

        var span = image.Memory.AsSpan(image.GetOffset(address, sizeof(uint)), sizeof(uint));
        value = BinaryPrimitives.ReadUInt32BigEndian(span);
        return true;
    }

    private static bool RegistersEqual(string left, string right) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private static uint AddSigned(uint value, int offset) =>
        unchecked(value + (uint)offset);

}
