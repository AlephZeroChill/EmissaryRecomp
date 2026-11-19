#!/usr/bin/env python3
"""Expansion gate: the build fails when translated code exceeds MAX host instructions per guest instruction.

    expansion_gate.py BINARY METADATA_JSON OBJDUMP [MAX=4.0]

Host instructions are counted per translated function symbol (func_XXXXXXXX, its func_XXXXXXXX_body when the
translator splits the patch guard into a wrapper, its func_XXXXXXXX_statefree clone, and any compiler-split .cold part)
in the linked binary; guest instructions come from the translator's per-function metadata (build.guestInstructions).
The gate is the total over all translated functions. On failure the binary is deleted, so a build over the cap
leaves nothing to run, and the worst offenders are listed by excess host instructions.
"""
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor


# A state-free clone (func_X_statefree) and the body behind a guard wrapper (func_X_body) are host code for the same
# guest instructions: they count toward func_X.
SYMBOL = re.compile(r"^[0-9a-f]+ <(func_[0-9A-Fa-f]{8})(?:_statefree|_body)?(?:\.cold[^>]*)?>:$")
OTHER = re.compile(r"^[0-9a-f]+ <")


def count_chunk(args):
    """Instructions per translated function in [start, stop) of the binary (one objdump process)."""
    binary, objdump, start, stop = args
    counts = {}
    current = None
    process = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", f"--start-address={start:#x}",
                                f"--stop-address={stop:#x}", binary], stdout=subprocess.PIPE, text=True,
                               bufsize=1 << 20)
    for line in process.stdout:
        if line.startswith(" ") or line.startswith("\t"):
            if current is not None and ":\t" in line:
                counts[current] = counts.get(current, 0) + 1
            continue
        match = SYMBOL.match(line.rstrip("\n"))
        if match:
            current = match.group(1)
        elif OTHER.match(line):
            current = None
    if process.wait() != 0:
        raise RuntimeError(f"{objdump} failed on {binary} [{start:#x}, {stop:#x})")
    return counts


def host_counts(binary, objdump):
    # One objdump over a few hundred MB takes minutes; split the code at function symbols into chunks and
    # disassemble them in parallel (MKW_GATE_JOBS, default: every CPU this process may use).
    nm = objdump[: -len("objdump")] + "nm" if objdump.endswith("objdump") else "nm"
    listing = subprocess.run([nm, "-n", "--defined-only", binary], capture_output=True, text=True, check=True).stdout
    addresses = [int(line.split()[0], 16) for line in listing.splitlines()
                 if len(line.split()) == 3 and line.split()[1] in "tT"]
    if not addresses:
        sys.exit(f"expansion gate: no code symbols in {binary}")
    jobs = int(os.environ.get("MKW_GATE_JOBS", len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity")
                              else os.cpu_count() or 1))
    chunks = max(1, jobs * 4)
    step = max(1, len(addresses) // chunks)
    bounds = addresses[::step] + [addresses[-1] + (1 << 24)]
    work = [(binary, objdump, bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]
    counts = {}
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for part in pool.map(count_chunk, work):
            for symbol, count in part.items():
                counts[symbol] = counts.get(symbol, 0) + count
    return counts


def load_guest(metadata_path):
    with open(metadata_path, encoding="utf-8") as handle:
        functions = json.load(handle)["functions"]
    guest = {}
    for function in functions:
        build = function.get("build") or {}
        count = build.get("guestInstructions") or 0
        if build.get("symbol") and count > 0:
            guest[build["symbol"]] = count
    if not guest:
        sys.exit("expansion gate: the metadata has no guestInstructions (retranslate with a translator that records them)")
    return guest


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--objects":
        # Quick sample: expansion_gate.py --objects METADATA OBJDUMP OBJ...  (object files, nothing deleted)
        guest = load_guest(sys.argv[2])
        host = {}
        with ProcessPoolExecutor() as pool:
            for part in pool.map(count_chunk, [(obj, sys.argv[3], 0, 1 << 62) for obj in sys.argv[4:]]):
                for symbol, count in part.items():
                    host[symbol] = host.get(symbol, 0) + count
        rows = sorted(((s, guest[s], h) for s, h in host.items() if s in guest), key=lambda r: r[2] - 4 * r[1],
                      reverse=True)
        g = sum(r[1] for r in rows)
        h = sum(r[2] for r in rows)
        print(f"sample: {len(rows)} functions, {g} guest -> {h} host, {h / max(g, 1):.2f}:1")
        for symbol, gi, hi in rows[:12]:
            print(f"  {symbol}  guest {gi:5d}  host {hi:7d}  {hi / gi:5.1f}:1")
        return 0
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    binary, metadata_path, objdump = sys.argv[1:4]
    cap = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
    # "report" (the default; MKW_ENFORCE_EXPANSION_GATE=OFF or MKW_TEST_BINARY): print the verdict, keep the binary.
    report_only = len(sys.argv) > 5 and sys.argv[5] == "report"
    guest = load_guest(metadata_path)

    host = host_counts(binary, objdump)
    rows = [(symbol, guest[symbol], host[symbol]) for symbol in guest if symbol in host]
    total_guest = sum(g for _, g, _ in rows)
    total_host = sum(h for _, _, h in rows)
    ratio = total_host / total_guest if total_guest else float("inf")
    print(f"expansion gate: {len(rows)} translated functions, {total_guest} guest -> {total_host} host instructions,"
          f" {ratio:.2f}:1 (cap {cap:.1f}:1)")
    buckets = [(0, 2), (2, 4), (4, 8), (8, 16), (16, float("inf"))]
    for low, high in buckets:
        part = [(g, h) for _, g, h in rows if low <= h / g < high]
        if part:
            g = sum(p[0] for p in part)
            h = sum(p[1] for p in part)
            print(f"  {low:>3}-{high:<4} {len(part):6d} functions  {100 * h / total_host:5.1f}% of host code"
                  f"  ratio {h / g:.1f}")
    if ratio <= cap:
        return 0
    worst = sorted(rows, key=lambda row: row[2] - cap * row[1], reverse=True)[:25]
    print(f"expansion gate: FAIL, {ratio:.2f}:1 exceeds {cap:.1f}:1. Worst offenders (excess host instructions):")
    for symbol, g, h in worst:
        print(f"  {symbol}  guest {g:5d}  host {h:7d}  {h / g:5.1f}:1  excess {h - cap * g:8.0f}")
    if report_only:
        print(f"expansion gate: report only, kept {binary} (MKW_ENFORCE_EXPANSION_GATE=ON deletes it)")
        return 0
    os.remove(binary)
    print(f"expansion gate: deleted {binary}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
