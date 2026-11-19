using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Disassembly;

/// <summary>
/// Simple disassembler that walks reachable instructions using the in-process PPC decoder.
/// Optimized for demand-driven decoding to avoid decoding unreachable instructions.
/// </summary>
public sealed class PpcDisassemblyLimitExceededException : InvalidOperationException
{
    public PpcDisassemblyLimitExceededException(
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        int decodedInstructions,
        uint blockedAddress)
        : base(
            $"Disassembly budget exhausted for 0x{entryPoint:X8} after {decodedInstructions} reachable instructions; " +
            $"next reachable address was 0x{blockedAddress:X8} (maxInstructions={maxInstructions}, maxBytes=0x{maxBytes:X}). " +
            "Increase the disassembly budget before trusting the generated control flow.")
    {
        EntryPoint = entryPoint;
        MaxInstructions = maxInstructions;
        MaxBytes = maxBytes;
        DecodedInstructions = decodedInstructions;
        BlockedAddress = blockedAddress;
    }

    public uint EntryPoint { get; }
    public int MaxInstructions { get; }
    public int MaxBytes { get; }
    public int DecodedInstructions { get; }
    public uint BlockedAddress { get; }
}

/// <summary>
/// Records every membership question asked of the known-function entry point set.
/// A cached decode is replayable only while all of these still answer the same way.
/// </summary>
public sealed class BoundaryProbeLog
{
    private List<uint>? _absent;

    /// <summary>Addresses that were asked about and were not boundaries.</summary>
    public IReadOnlyList<uint> AbsentBoundaries => (IReadOnlyList<uint>?)_absent ?? Array.Empty<uint>();

    public void RecordAbsent(uint address)
    {
        _absent ??= new List<uint>();
        if (!_absent.Contains(address))
        {
            _absent.Add(address);
        }
    }

    /// <summary>
    /// True while every recorded question still has the same answer. Boundary
    /// sets only ever grow during a run, so only the negative answers can flip.
    /// </summary>
    public bool IsStillValid(IReadOnlySet<uint>? knownFunctionEntryPoints)
    {
        if (_absent is null || knownFunctionEntryPoints is null)
        {
            return true;
        }

        for (var i = 0; i < _absent.Count; i++)
        {
            if (knownFunctionEntryPoints.Contains(_absent[i]))
            {
                return false;
            }
        }

        return true;
    }
}

public sealed class PpcDisassembler : IDisposable
{
    public IReadOnlyList<PpcInstruction> DisassembleFunction(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions = 256,
        int maxBytes = 0x800,
        IReadOnlySet<uint>? knownFunctionEntryPoints = null,
        BoundaryProbeLog? boundaryProbes = null,
        IReadOnlyList<AddressRange>? inlineCodeRegions = null)
    {
        var instructions = DisassembleFunctionCore(
            image, entryPoint, maxInstructions, maxBytes, knownFunctionEntryPoints, boundaryProbes, throwOnBudget: true,
            inlineCodeRegions);
        return instructions!;
    }

    /// <summary>
    /// Budget-tolerant variant for speculative decoding (leaf inlining), where an
    /// oversized body is a normal "not a candidate" answer, not an error to throw and swallow.
    /// </summary>
    public IReadOnlyList<PpcInstruction>? TryDisassembleFunction(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        IReadOnlySet<uint>? knownFunctionEntryPoints = null,
        BoundaryProbeLog? boundaryProbes = null)
        => DisassembleFunctionCore(
            image, entryPoint, maxInstructions, maxBytes, knownFunctionEntryPoints, boundaryProbes, throwOnBudget: false);

    private static IReadOnlyList<PpcInstruction>? DisassembleFunctionCore(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        IReadOnlySet<uint>? knownFunctionEntryPoints,
        BoundaryProbeLog? boundaryProbes,
        bool throwOnBudget,
        IReadOnlyList<AddressRange>? inlineCodeRegions = null)
    {
        if (!image.Contains(entryPoint, sizeof(uint)))
        {
            throw new ArgumentOutOfRangeException(nameof(entryPoint), $"Address 0x{entryPoint:X8} is outside the loaded RAM image.");
        }
        var offset = image.GetOffset(entryPoint, sizeof(uint));

        var available = Math.Min(maxBytes, image.Memory.Length - offset);

        // Demand-driven decoding: decode only the instructions we actually visit
        var instructionMap = new Dictionary<uint, PpcInstruction>();
        var reachable = new HashSet<uint>();
        var localControlFlowTargets = new HashSet<uint>();
        var worklist = new Queue<uint>();
        worklist.Enqueue(entryPoint);

        var endAddress = entryPoint + (uint)available;

        // Ordered view used by jump-table recognition, rebuilt lazily when the decoded set grows.
        List<PpcInstruction>? orderedCache = null;
        Dictionary<uint, int>? indexByAddressCache = null;
        var orderedCacheCount = -1;

        while (worklist.Count > 0)
        {
            if (reachable.Count >= maxInstructions)
            {
                if (!throwOnBudget)
                {
                    return null;
                }

                throw new PpcDisassemblyLimitExceededException(
                    entryPoint,
                    maxInstructions,
                    maxBytes,
                    reachable.Count,
                    worklist.Peek());
            }

            var cursor = worklist.Dequeue();
            while ((cursor >= entryPoint && cursor < endAddress) || InInlineRegion(cursor))
            {
                if (reachable.Count >= maxInstructions)
                {
                    if (!throwOnBudget)
                    {
                        return null;
                    }

                    throw new PpcDisassemblyLimitExceededException(
                        entryPoint,
                        maxInstructions,
                        maxBytes,
                        reachable.Count,
                        cursor);
                }

                // Decode on demand
                if (!instructionMap.TryGetValue(cursor, out var ins))
                {
                    if (!image.Contains(cursor, sizeof(uint)))
                    {
                        break;
                    }
                    var cursorOffset = image.GetOffset(cursor, sizeof(uint));
                    var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(cursorOffset, 4));
                    ins = PpcDecoder.Decode(cursor, word);
                    instructionMap[cursor] = ins;
                }

                if (!reachable.Add(ins.Address))
                {
                    break;
                }

                // In inline code regions (a Gecko code list's hook bodies), "bl over data, then mflr" fetches the
                // address of data embedded in the code: the bl is no call, it sets lr and jumps on, and what follows
                // it is data. As bl_jump it is decoded as such: lr = next address, jump to the target. (A call would
                // hand the rest of the body to the interpreter with lr pointing at the data, and the frame would
                // never return.)
                // "bl T" where T is "blrl" fetches an address the other way round: the blrl goes straight back to the
                // instruction after the bl with lr = T + 4 (Project+'s data.debug loader then reads the string after
                // T). As bl_blrl: lr = T + 4, continue at the next instruction.
                if (inlineCodeRegions is not null && ins.IsCall && !ins.IsConditionalBranch &&
                    ins.BranchTargets.Count == 1 && ins.Mnemonic.Equals("bl", StringComparison.OrdinalIgnoreCase) &&
                    InInlineRegion(ins.Address) && InInlineRegion(ins.BranchTargets[0]) &&
                    image.Contains(ins.BranchTargets[0], sizeof(uint)) &&
                    BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(
                        image.GetOffset(ins.BranchTargets[0], sizeof(uint)), 4)) == 0x4E800021u)
                {
                    ins = new PpcInstruction(ins.Address, ins.Word, "bl_blrl", ins.Operands, new[] { ins.EndAddress },
                        isReturn: false, isCall: false, isConditionalBranch: false);
                    instructionMap[ins.Address] = ins;
                }

                if (inlineCodeRegions is not null && ins.IsCall && !ins.IsConditionalBranch &&
                    ins.BranchTargets.Count == 1 && ins.Mnemonic.Equals("bl", StringComparison.OrdinalIgnoreCase) &&
                    ins.BranchTargets[0] != ins.EndAddress && InInlineRegion(ins.Address) &&
                    InInlineRegion(ins.BranchTargets[0]) && ReadsLinkAsDataAddress(ins.BranchTargets[0]))
                {
                    ins = new PpcInstruction(ins.Address, ins.Word, "bl_jump", ins.Operands, ins.BranchTargets,
                        isReturn: false, isCall: false, isConditionalBranch: false);
                    instructionMap[ins.Address] = ins;
                }

                // In inline code regions, a hook body that jumps back with "lis rX,hi; ori rX,rX,lo (or addi);
                // mtctr rX; bctr" goes to a constant address, often the middle of its own function: decoded as that
                // direct branch (a local jump there), not as an indirect tail call that would enter the function's
                // middle in the interpreter with a stale lr.
                if (inlineCodeRegions is not null && ins.BranchTargets.Count == 0 &&
                    ins.Mnemonic.Equals("bctr", StringComparison.OrdinalIgnoreCase) && InInlineRegion(ins.Address) &&
                    ConstantCtrTarget(ins.Address) is { } ctrTarget)
                {
                    ins = new PpcInstruction(ins.Address, ins.Word, "b", ins.Operands, new[] { ctrTarget },
                        isReturn: false, isCall: false, isConditionalBranch: false);
                    instructionMap[ins.Address] = ins;
                }

                var recognizedJumpTable = false;
                if (ins.BranchTargets.Count == 0 && string.Equals(ins.Mnemonic, "bctr", StringComparison.OrdinalIgnoreCase))
                {
                    // For jump tables, we need the ordered list - build it lazily
                    if (orderedCache is null || indexByAddressCache is null || orderedCacheCount != instructionMap.Count)
                    {
                        orderedCache = instructionMap.Values.OrderBy(i => i.Address).ToList();
                        indexByAddressCache = new Dictionary<uint, int>(orderedCache.Count);
                        for (var idx = 0; idx < orderedCache.Count; idx++)
                        {
                            indexByAddressCache[orderedCache[idx].Address] = idx;
                        }

                        orderedCacheCount = instructionMap.Count;
                    }

                    // Code variants (inline code regions) also trace a switch's bound compare through a mod's hook body.
                    if (JumpTableDetector.TryRecognize(orderedCache, indexByAddressCache, ins.Address, entryPoint, endAddress, image, out var jtTargets,
                            followControlFlow: inlineCodeRegions is not null))
                    {
                        ins = new PpcInstruction(ins.Address, ins.Word, ins.Mnemonic, ins.Operands, jtTargets, isReturn: false, ins.IsCall, ins.IsConditionalBranch);
                        instructionMap[ins.Address] = ins;
                        // Same address, so the ordered view's count is unchanged but its content is stale; drop it.
                        orderedCache = null;
                        indexByAddressCache = null;
                        orderedCacheCount = -1;
                        foreach (var target in jtTargets)
                        {
                            localControlFlowTargets.Add(target);
                        }
                        recognizedJumpTable = true;
                    }
                }

                if (ins.IsConditionalCall)
                {
                    cursor = ins.EndAddress;  // a call: its target is another function, execution continues here
                    continue;
                }

                if (ins.IsConditionalBranch)
                {
                    foreach (var target in ins.BranchTargets)
                    {
                        localControlFlowTargets.Add(target);
                        if (!reachable.Contains(target))
                        {
                            worklist.Enqueue(target);
                        }
                    }
                    cursor = ins.EndAddress;
                    continue;
                }

                if (ins.IsUnconditionalBranch)
                {
                    foreach (var target in ins.BranchTargets)
                    {
                        if ((recognizedJumpTable || !IsKnownExternalFunctionEntry(target)) && !reachable.Contains(target))
                        {
                            worklist.Enqueue(target);
                        }
                    }
                    break; // no fallthrough
                }

                if (ins.IsReturn)
                {
                    break;
                }

                cursor = ins.EndAddress;
            }
        }

        var addresses = new uint[reachable.Count];
        reachable.CopyTo(addresses);
        Array.Sort(addresses);
        var ordered = new List<PpcInstruction>(addresses.Length);
        foreach (var address in addresses)
        {
            ordered.Add(instructionMap[address]);
        }

        return ordered;

        // Whether the code a bl reaches at `target` takes lr as an address (the "bl over data" idiom) rather than being
        // a subroutine: along its straight line (conditional branches fall through), the first mflr comes before any
        // call, return or unconditional branch, and its register is not then saved to the stack (a prologue's
        // "mflr r0; stw r0, d(r1)").
        bool ReadsLinkAsDataAddress(uint target)
        {
            uint Word(uint at) => image.Contains(at, sizeof(uint))
                ? BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(at, sizeof(uint)), 4))
                : 0u;
            for (var at = target; at < target + 8 * 4; at += 4)
            {
                var word = Word(at);
                var primary = word >> 26;
                if ((word & 0xFC1FFFFFu) == 0x7C0802A6u)  // mflr rD
                {
                    var rd = (word >> 21) & 31;
                    for (var next = at + 4; next < at + 4 * 4; next += 4)
                    {
                        var use = Word(next);
                        // stw/stwu rD, d(r1)
                        if ((use >> 26 is 36 or 37) && ((use >> 21) & 31) == rd && ((use >> 16) & 31) == 1) return false;
                    }
                    return true;
                }
                if (primary == 18) return false;                               // b, bl
                if (primary == 16 && (word & 1) != 0) return false;            // bcl
                if (primary == 19 && (((word >> 1) & 0x3FF) is 16 or 528)) return false;  // bclr, bcctr
            }
            return false;
        }

        // The constant a "lis rX,hi; ori rX,rX,lo (or addi rX,rX,lo); mtctr rX" right before a bctr at `bctr` loads.
        uint? ConstantCtrTarget(uint bctr)
        {
            uint Word(uint at) => image.Contains(at, sizeof(uint))
                ? BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(at, sizeof(uint)), 4))
                : 0u;
            var mtctr = Word(bctr - 4);
            var low = Word(bctr - 8);
            var high = Word(bctr - 12);
            if ((mtctr & 0xFC1FFFFFu) != 0x7C0903A6u) return null;
            var reg = (mtctr >> 21) & 31;
            // lis rX, hi = addis rX, 0, hi
            if (high >> 26 != 15 || ((high >> 21) & 31) != reg || ((high >> 16) & 31) != 0) return null;
            var upper = (high & 0xFFFFu) << 16;
            if (low >> 26 == 24 && ((low >> 21) & 31) == reg && ((low >> 16) & 31) == reg)  // ori rX, rX, lo
                return upper | (low & 0xFFFFu);
            if (low >> 26 == 14 && ((low >> 21) & 31) == reg && ((low >> 16) & 31) == reg)  // addi rX, rX, lo
                return upper + (uint)(short)(low & 0xFFFFu);
            return null;
        }

        // Code a code variant's branches enter outside the function's own range (FunctionTranslator's
        // InlineCodeRegions); decoded as the function's own blocks.
        bool InInlineRegion(uint address)
        {
            if (inlineCodeRegions is null)
            {
                return false;
            }

            foreach (var region in inlineCodeRegions)
            {
                if (address >= region.Start && address < region.End)
                {
                    return true;
                }
            }

            return false;
        }

        bool IsKnownExternalFunctionEntry(uint target)
        {
            if (target == entryPoint || localControlFlowTargets.Contains(target))
            {
                return false;
            }

            if (knownFunctionEntryPoints is null)
            {
                return false;
            }

            var known = knownFunctionEntryPoints.Contains(target);
            if (!known)
            {
                boundaryProbes?.RecordAbsent(target);
            }

            return known;
        }
    }

    public void Dispose()
    {
        // nothing to dispose currently
    }
}
