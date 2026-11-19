#!/usr/bin/env python3
"""gen_variant_spec.py: the code-variant spec (translate-recursive --code-variants) for a mod's fixed code patches, from
memory dumps of a running game (BRAWL_DUMP_ON_SCENE) taken once the mod's code handler has patched the game.

  gen_variant_spec.py WIICOMPILED_DIR DUMP_PREFIX OUT_DIR [--region START-END]...

WIICOMPILED_DIR: the tree whose translation the variants join (../game/main_translate.dol, the resident data ranges,
and generated/build_shards/base_dispatch/guest_code_guard_*.cpp for the translated function starts and inlined leaves).
DUMP_PREFIX: <prefix>_<START>.bin files covering main.dol's code, the resident modules and each --region.
--region: memory the patched code branches into, default Project+'s two code lists and Syriinge's heap.
--patched-log: the capture run's runtime log: every function it reports patched gets a variant too.
--code-region START-END: a mod's own code linked in place (a replacement module, a plugin): a region whose functions
  get variants of their own, found from --entries and the calls and tail branches they make inside code regions.
  Dump these after the scene has loaded them (pad script MEMDUMP with BRAWL_DUMP_RANGES, hle/input/pad.cpp).
--entries FILE: an interpreter profile dump (MKW_INTERP_PROFILE build, BRAWL_INTERP_PROFILE): its entries inside
  code regions seed them.

The translation needs generated/ from a first translate-recursive run (the guard table): translate once without
--code-variants, run this, then translate again with it. For Project+ the dumps come from a Training run at its first
scMelee frame, e.g. BRAWL_DUMP_ON_SCENE=scMelee:80001800-80003000,80550000-80566000,80566000-80580000,
80004000-80406500,80673F00-80B85000,81050000-81070000:<prefix> (hle/input/pad.cpp).

Writes OUT_DIR/spec.txt (+ the region dumps beside it):
  W addr word   every word of translated code that differs from the translation's image (the mod's patch)
  F addr        each translated function holding one, and each caller holding an inlined copy of one
  R start end file
  X pc / C target   PCs and call targets runtime/src/ppc_interpreter.cpp handles beyond the guest code
"""
import glob, re, struct, sys
from pathlib import Path


def read_dol_text(path):
    d = Path(path).read_bytes()
    h = struct.unpack(">18I18I18I", d[:0xD8])
    secs = []
    for i in range(7):
        off, addr, size = h[i], h[18 + i], h[36 + i]
        if size:
            secs.append((addr, addr + size, d[off:off + size]))
    return secs


def main():
    wc = Path(sys.argv[1]); prefix = sys.argv[2]; out = Path(sys.argv[3])
    regions = []
    code_regions = []
    entry_files = []
    patched_log = None
    args = sys.argv[4:]
    while args:
        if args[0] == "--region":
            a, b = args[1].split("-"); regions.append((int(a, 16), int(b, 16))); args = args[2:]
        elif args[0] == "--code-region":
            a, b = args[1].split("-"); code_regions.append((int(a, 16), int(b, 16))); args = args[2:]
        elif args[0] == "--entries":
            entry_files.append(Path(args[1])); args = args[2:]
        elif args[0] == "--patched-log":
            patched_log = Path(args[1]); args = args[2:]
        else:
            sys.exit(f"unknown argument {args[0]}")
    if not regions:
        # Project+'s two code lists (BOOST.GCT at 0x80550010, RSBE01.GCT at gc.txt's 0x80566528) and Syriinge's heap
        # (sy_core's hook trampolines at 0x8105E000-0x81061000).
        regions = [(0x80550000, 0x80566000), (0x80566000, 0x80580000), (0x81050000, 0x81070000)]
    out.mkdir(parents=True, exist_ok=True)

    dumps = {}
    for p in glob.glob(prefix + "_*.bin"):
        start = int(re.search(r"_([0-9A-F]{8})\.bin$", p)[1], 16)
        dumps[start] = Path(p).read_bytes()

    def dump_word(addr):
        for start, data in dumps.items():
            if start <= addr and addr + 4 <= start + len(data):
                return struct.unpack(">I", data[addr - start:addr - start + 4])[0]
        return None

    data_ranges = []
    for line in (wc / "projects/brawl/resident_data_ranges.txt").read_text().splitlines():
        f = line.split()
        if len(f) >= 2 and not line.startswith("#"):
            data_ranges.append((int(f[0], 16), int(f[1], 16)))

    guard = glob.glob(str(wc / "generated/build_shards/base_dispatch/guest_code_guard_*.cpp"))[0]
    text = Path(guard).read_text()
    starts_block = text.split("g_mkwGuardedFunctionStarts[] = {")[1].split("};")[0]
    starts = [int(x, 16) for x in re.findall(r"0x([0-9A-F]{8})u", starts_block)]
    leaves_block = text.split("g_mkwInlinedLeaves[] = {")[1].split("};")[0]
    pairs = [int(x) for x in re.findall(r"(\d+)u", leaves_block)]
    inlined = {}  # leaf index -> caller indices
    for i in range(0, len(pairs) - 1, 2):
        inlined.setdefault(pairs[i], []).append(pairs[i + 1])

    import bisect
    words = {}
    for lo, hi, data in read_dol_text(wc.parent / "game/main_translate.dol"):
        # build_modules.py's resident veneer slot is sora_melee's bss at run time (data, never guarded code).
        if lo == 0x80B84DC0:
            continue
        for addr in range(lo, hi, 4):
            if any(a <= addr < b for a, b in data_ranges):
                continue
            now = dump_word(addr)
            if now is None:
                continue
            was = struct.unpack(">I", data[addr - lo:addr - lo + 4])[0]
            if now != was:
                words[addr] = now

    patched = set()
    for addr in words:
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0:
            patched.add(i)
    callers = set()
    for i in patched:
        callers.update(c for c in inlined.get(i, []) if c not in patched)
    functions = sorted(starts[i] for i in patched | callers)
    # --patched-log: the capture run's log. A function the runtime marked patched stays in the interpreter for good,
    # also once its code is back to the disc's (Project+ moves its code handler hook between __AXOutNewFrame and
    # VISetNextFrameBuffer): it gets a variant of its state in the dumps as well.
    if patched_log is not None:
        start_set = set(starts)
        marked = set(int(a, 16) for a in re.findall(
            r"function 0x([0-9A-F]{8}) (?:now runs in the interpreter|inlined)", patched_log.read_text(errors="replace")))
        functions = sorted(set(functions) | (marked & start_set))

    # Continuations: a plugin loader that replaces a whole function (Syriinge) calls the original through a stub in its
    # heap, "<the function's first instruction>; b <its second>". The variant entered there runs the original body.
    original = {}
    for lo, hi, data in read_dol_text(wc.parent / "game/main_translate.dol"):
        for i in patched:
            if lo <= starts[i] < hi:
                original[starts[i]] = struct.unpack(">I", data[starts[i] - lo:starts[i] - lo + 4])[0]
    continuations = set()
    for lo, hi in regions:
        for addr in range(lo + 4, hi, 4):
            word = dump_word(addr)
            if word is None or word >> 26 != 18 or word & 3:
                continue
            disp = word & 0x03FFFFFC
            target = addr + (disp - 0x04000000 if disp & 0x02000000 else disp)
            if original.get(target - 4) == dump_word(addr - 4) and target - 4 in original:
                continuations.add(target)
    functions = sorted(set(functions) | continuations)

    # Code regions: the profiled entries inside them, then every function those call or tail-branch to inside them,
    # walked statically from the dumps (a function's own branches are followed until blr / bctr / a branch out).
    def in_code(addr):
        return any(lo <= addr < hi for lo, hi in code_regions)

    def branch_target(addr, word):
        if word >> 26 == 18:
            disp = word & 0x03FFFFFC
            disp = disp - 0x04000000 if disp & 0x02000000 else disp
            return (disp if word & 2 else addr + disp) & 0xFFFFFFFF
        if word >> 26 == 16:
            disp = word & 0xFFFC
            disp = disp - 0x10000 if disp & 0x8000 else disp
            return (disp if word & 2 else addr + disp) & 0xFFFFFFFF
        return None

    seeds = set()
    for path in entry_files:
        for line in path.read_text(errors="replace").splitlines():
            f = line.split()
            if len(f) >= 2 and f[0] == "E" and in_code(int(f[1], 16)):
                seeds.add(int(f[1], 16))
    module_functions = set()
    work = sorted(seeds)
    while work:
        entry = work.pop()
        if entry in module_functions or not in_code(entry):
            continue
        module_functions.add(entry)
        seen, todo = set(), [entry]
        while todo and len(seen) < 8192:
            at = todo.pop()
            while in_code(at) and at not in seen and at - entry < 0x10000:
                seen.add(at)
                word = dump_word(at)
                if word is None:
                    break
                op = word >> 26
                target = branch_target(at, word)
                if op == 18 and word & 1:  # bl: a call
                    if target is not None and in_code(target):
                        work.append(target)
                    at += 4
                    continue
                if op == 18:  # b: inside the function, or a tail call to another
                    if target is not None and in_code(target):
                        if entry <= target < entry + 0x10000 and target > at - 0x4000:
                            todo.append(target)
                        else:
                            work.append(target)
                    break
                if op == 16:  # bc: both ways (bcl: a call)
                    if target is not None and in_code(target):
                        (work if word & 1 else todo).append(target)
                    at += 4
                    continue
                if op == 19 and ((word >> 1) & 0x3FF) in (16, 528) and (word >> 21) & 0x14 == 0x14:
                    break  # blr / bctr (unconditional)
                at += 4
    functions = sorted(set(functions) | module_functions)

    lines = ["# generated by gen_variant_spec.py from " + prefix]
    lines += [f"W {a:08X} {w:08X}" for a, w in sorted(words.items())]
    lines += [f"F {a:08X}" for a in functions]
    # runtime/src/ppc_interpreter.cpp does more than the guest code at these PCs (Brawl Online's rollback main loop,
    # its stage hooks) and around calls of these targets (BrawlRollback::Run / UpdateSystem): functions reaching them
    # stay interpreted.
    lines += [f"X {pc:08X}" for pc in (0x800171B4, 0x80017508, 0x800172D0, 0x8001730C, 0x80949BC0, 0x809435F0)]
    lines += [f"C {t:08X}" for t in (0x80017618, 0x8002A210)]
    for lo, hi in regions + code_regions:
        src = next((s for s in dumps if s <= lo and hi <= s + len(dumps[s])), None)
        if src is None:
            sys.exit(f"no dump covers region {lo:08X}-{hi:08X}")
        name = f"region_{lo:08X}.bin"
        (out / name).write_bytes(dumps[src][lo - src:hi - src])
        lines.append(f"R {lo:08X} {hi:08X} {name}")
    (out / "spec.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(words)} patched words in {len(patched)} functions, {len(callers)} inlining callers, "
          f"{len(continuations)} continuations, {len(module_functions)} functions in code regions "
          f"({len(seeds)} profiled entries), {len(functions)} variants requested, {len(regions) + len(code_regions)} regions")


if __name__ == "__main__":
    main()
