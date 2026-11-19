using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

/// <summary>
/// SSA values that some path carries across a guest call that may change their register, before a use. The front end
/// lowers a guest call to an <see cref="IrCall"/> / <see cref="IrIndirectCall"/> with no destination, so SSA gives no
/// register a new version at the call: a read of r3 after "bl" names the value r3 held before it, while the emitted
/// code reads what the call left there (it reloads the callee's possible writes from the context). The facts keyed by
/// SSA name (<c>CxxLinearCodeGenerator.BuildKnownUIntConstants</c>, <see cref="StackAddressFacts"/>) must not be used at
/// such a read: "r3 = 1324; bl operator new; mr r30, r3; stw ..., 60(r30)" made the store a proven-RAM (check-free)
/// store to 0x568 although it writes the new object. Per use: the builders derive nothing from a stale read, and the
/// readers of a fact check their own read (<see cref="IsStaleUse"/>); the same value keeps its facts where it is read
/// before the call.
/// <para>A call changes the EABI's volatile GPRs (r0, r3-r12) that its callee's ABI contract may write (the emitter
/// reloads the contract's possible writes after it: <c>ResidencyBoundarySync.FromCalleeContract</c>; nonvolatiles a
/// callee writes it also restores); without a usable contract (none, a mod-overridable target, an indirect call) all of
/// r0 and r3-r12. The contracts come from <see cref="UseCallContracts"/> (set per emitted function); without them every
/// call takes the full set.</para>
/// <para>Redefining the registers in SSA itself would say the same, but the ABI inference and the emitter's register
/// residency read those versions too: done that way, 7,808 functions changed their inferred contracts and post-call
/// reloads (an earlier attempt).</para>
/// </summary>
internal static class CallClobberedValues
{
    private const uint EabiVolatileGprs = 0x1FF9u;  // r0, r3-r12
    private const uint NeverClobbered = (1u << 1) | (1u << 2) | (1u << 13);  // stack and small-data pointers

    [ThreadStatic] private static IReadOnlyDictionary<uint, GuestAbiContract>? _contracts;
    [ThreadStatic] private static IReadOnlySet<uint>? _overridable;

    private sealed record CacheEntry(object? Contracts, object? Overridable);
    private static readonly ConditionalWeakTable<IrFunction, CacheEntry> Analyzed = new();

    /// <summary>
    /// Per instruction, the names it reads that a call may have changed since their definition (stale uses). Keyed by
    /// the instruction object: after SSA renaming each function owns its instruction objects, and the passes after it
    /// keep an unchanged instruction's object, so a lookup from any later version of the function finds its entry.
    /// </summary>
    private static readonly ConditionalWeakTable<IrInstruction, HashSet<string>> StaleAt = new();

    /// <summary>The callee contracts (and mod-overridable targets) the next analyses on this thread use.</summary>
    public static void UseCallContracts(IReadOnlyDictionary<uint, GuestAbiContract>? contracts, IReadOnlySet<uint>? overridable)
    {
        _contracts = contracts;
        _overridable = overridable;
    }

    /// <summary>
    /// Records the stale uses of <paramref name="function"/>'s instructions (once per function object and contract
    /// tables); the fact builders call it before reading <see cref="IsStaleUse"/> / <see cref="HasStaleUse"/>.
    /// </summary>
    public static void Analyze(IrFunction function)
    {
        var contracts = _contracts;
        var overridable = _overridable;
        if (Analyzed.TryGetValue(function, out var cached) &&
            ReferenceEquals(cached.Contracts, contracts) && ReferenceEquals(cached.Overridable, overridable))
        {
            return;
        }
        Compute(function, contracts, overridable);
        Analyzed.AddOrUpdate(function, new CacheEntry(contracts, overridable));
    }

    /// <summary>
    /// Whether <paramref name="instruction"/> reads <paramref name="name"/> where some path carries it across a call
    /// that may change its register: the value read is not the one the name was defined with.
    /// </summary>
    public static bool IsStaleUse(IrInstruction instruction, string? name) =>
        name is not null && StaleAt.TryGetValue(instruction, out var names) && names.Contains(name);

    /// <summary>Whether any register <paramref name="instruction"/> reads is a stale use.</summary>
    public static bool HasStaleUse(IrInstruction instruction) => StaleAt.TryGetValue(instruction, out _);

    /// <summary>The GPRs (bit n = rn) a guest call may change; 0 for anything that is not a guest call.</summary>
    private static uint CallWriteMask(IrInstruction instruction, IReadOnlyDictionary<uint, GuestAbiContract>? contracts,
        IReadOnlySet<uint>? overridable)
    {
        switch (instruction)
        {
            case IrCall call when string.IsNullOrWhiteSpace(call.Destination) &&
                                  GuestTargetParser.TryParseAddress(call.Target, out var target):
                if (contracts is not null && contracts.TryGetValue(target, out var contract) &&
                    overridable?.Contains(target) != true)
                {
                    // The possible-write mask also lists the nonvolatiles a callee saves and restores; after the call
                    // those hold their old values (the EABI, which the fact tables trust as the plan does).
                    var mask = contract.GprPossibleWriteMask & EabiVolatileGprs;
                    if (contract.HasFullSynchronizationFence) mask |= EabiVolatileGprs;
                    return mask;
                }
                return EabiVolatileGprs;
            case IrIndirectCall icall when string.IsNullOrWhiteSpace(icall.Destination):
                return EabiVolatileGprs;
            default:
                return 0;
        }
    }

    private static int GprIndex(string name)
    {
        var hardware = RegisterNameUtils.HardwareBase(name);
        return hardware.Length is >= 2 and <= 3 && hardware[0] == 'r' &&
               int.TryParse(hardware.AsSpan(1), out var index) && index is >= 0 and < 32 &&
               (NeverClobbered & (1u << index)) == 0
            ? index
            : -1;
    }

    private static void Compute(IrFunction function, IReadOnlyDictionary<uint, GuestAbiContract>? contracts,
        IReadOnlySet<uint>? overridable)
    {
        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions) StaleAt.Remove(instruction);
        }
        var staleUses = new Dictionary<IrInstruction, HashSet<string>>(ReferenceEqualityComparer.Instance);
        void Record(IrInstruction instruction, string name)
        {
            if (!staleUses.TryGetValue(instruction, out var set))
            {
                staleUses[instruction] = set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            }
            set.Add(name);
        }
        var hasCall = false;
        var index = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var names = new List<string>();
        var registerOf = new List<int>();
        void Intern(string name)
        {
            if (index.ContainsKey(name)) return;
            var gpr = GprIndex(name);
            if (gpr < 0) return;
            index[name] = names.Count;
            names.Add(name);
            registerOf.Add(gpr);
        }
        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                hasCall |= CallWriteMask(instruction, contracts, overridable) != 0;
                foreach (var name in IrRegisterDataFlow.Uses(instruction)) Intern(name);
                foreach (var name in IrRegisterDataFlow.Definitions(instruction)) Intern(name);
            }
        }
        if (!hasCall || names.Count == 0) return;

        // Forward may-analysis over the CFG: the names that may have crossed a call changing their register since
        // their definition (or since entry). A call adds the names of the registers it may write; a definition
        // removes its own name. A use of a name in the set is a stale use: no fact holds there, nor for a value
        // computed from it. The same name keeps its facts at its other uses (before the call, say).
        var words = (names.Count + 63) >> 6;
        var byRegister = new ulong[32][];
        for (var id = 0; id < names.Count; id++)
        {
            var bits = byRegister[registerOf[id]] ??= new ulong[words];
            bits[id >> 6] |= 1UL << id;
        }
        var cfg = IrCfg.Build(function);
        var blockIndex = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (var i = 0; i < function.Blocks.Count; i++) blockIndex[function.Blocks[i].Label] = i;
        var inSets = new ulong[function.Blocks.Count][];
        var outSets = new ulong[function.Blocks.Count][];

        bool Has(ulong[] set, string name) =>
            index.TryGetValue(name, out var id) && (set[id >> 6] & (1UL << id)) != 0;

        ulong[] Transfer(int b, ulong[] input, bool record)
        {
            var set = (ulong[])input.Clone();
            var block = function.Blocks[b];
            foreach (var instruction in block.Instructions)
            {
                if (record && instruction is not IrPhi)  // a phi's sources are read on the incoming edges (below)
                {
                    foreach (var name in IrRegisterDataFlow.Uses(instruction))
                    {
                        if (Has(set, name)) Record(instruction, name);
                    }
                }
                foreach (var name in IrRegisterDataFlow.Definitions(instruction))
                {
                    if (index.TryGetValue(name, out var id)) set[id >> 6] &= ~(1UL << id);
                }
                var mask = CallWriteMask(instruction, contracts, overridable);
                for (var r = 0; mask != 0 && r < 32; r++, mask >>= 1)
                {
                    if ((mask & 1) == 0 || byRegister[r] is not { } bits) continue;
                    for (var w = 0; w < words; w++) set[w] |= bits[w];
                }
            }
            if (record)
            {
                foreach (var successor in cfg.Successors(block.Label))
                {
                    if (!blockIndex.TryGetValue(successor, out var s)) continue;
                    foreach (var instruction in function.Blocks[s].Instructions)
                    {
                        if (instruction is IrPhi phi && phi.Sources.TryGetValue(block.Label, out var source) && Has(set, source))
                        {
                            Record(phi, source);
                        }
                    }
                }
            }
            return set;
        }

        var work = new Queue<int>();
        var queued = new bool[function.Blocks.Count];
        for (var i = 0; i < function.Blocks.Count; i++)
        {
            inSets[i] = new ulong[words];
            work.Enqueue(i);
            queued[i] = true;
        }
        while (work.Count > 0)
        {
            var b = work.Dequeue();
            queued[b] = false;
            var output = Transfer(b, inSets[b], record: false);
            if (outSets[b] is { } previous && SameSet(previous, output)) continue;
            outSets[b] = output;
            foreach (var successor in cfg.Successors(function.Blocks[b].Label))
            {
                if (!blockIndex.TryGetValue(successor, out var s)) continue;
                var changed = false;
                for (var w = 0; w < words; w++)
                {
                    var merged = inSets[s][w] | output[w];
                    changed |= merged != inSets[s][w];
                    inSets[s][w] = merged;
                }
                if ((changed || outSets[s] is null) && !queued[s])
                {
                    work.Enqueue(s);
                    queued[s] = true;
                }
            }
        }
        for (var b = 0; b < function.Blocks.Count; b++) Transfer(b, inSets[b], record: true);
        foreach (var (instruction, set) in staleUses) StaleAt.AddOrUpdate(instruction, set);
    }

    private static bool SameSet(ulong[] left, ulong[] right)
    {
        for (var i = 0; i < left.Length; i++)
        {
            if (left[i] != right[i]) return false;
        }
        return true;
    }
}
