#!/usr/bin/env python3
"""interp_report.py RUNTIME_LOG PROFILE.txt [PROFILE.txt...]: where interpreted guest code runs, from MKW_INTERP_PROFILE dumps
(ppc_interpreter.cpp ProfileDump) of one run, classified by the guest memory region of each entry and PC.

Regions come from the run's stdout (module link lines) plus Brawl/P+'s fixed layout. Per entry: calls, guest
instructions and exclusive host ticks (rdtsc) per frame; "patched" = the entry has a translation (a hooked or redirected
function the interpreter runs instead)."""
import re, sys
from collections import defaultdict

FIXED = [
    (0x80001800, 0x800022A8, "gecko handler"),
    (0x80004000, 0x804064E0, "main.dol"),
    (0x80550010, 0x80566000, "BOOST.GCT list"),
    (0x80566528, 0x80580000, "RSBE01.GCT list"),
    (0x80673FA0, 0x80680AA0, "res sora_menu_name"),
    (0x80680C40, 0x80682720, "res sora_menu_sel_char_access"),
    (0x80682800, 0x806A3960, "res sora_menu_sel_char"),
    (0x806A3A60, 0x806B07E0, "res sora_menu_rule"),
    (0x806B08C0, 0x806BB380, "res sora_menu_sel_stage"),
    (0x806BB480, 0x80708C40, "res sora_scene"),
    (0x8070A940, 0x80B84DC0, "res sora_melee"),
]
PLUGINS = {224: "syr sy_core", 202: "syr AsyncRSP", 8192: "syr lavaInjectLoader", 8195: "syr lavaNeutralSpawns",
           20561: "syr PPOnline(loader)", 20560: "syr PPOnline"}


def regions_from_log(path):
    names = {}  # id -> name
    spans = []  # (start, end, label), latest link wins
    for line in open(path, errors="replace"):
        m = re.search(r"REL module (\S+) at 0x([0-9A-F]+) is not the disc's", line)
        if m:
            names[int(m[2], 16)] = m[1]
        m = re.search(r"module (\S+) linked at 0x([0-9A-F]+): calls into its home", line)
        if m:
            names[int(m[2], 16) & ~0xFF] = m[1]
        m = re.search(r"REL module (\d+) linked at image 0x([0-9A-F]+) bss 0x([0-9A-F]+)", line)
        if m:
            mid, image, bss = int(m[1]), int(m[2], 16), int(m[3], 16)
            label = PLUGINS.get(mid) or names.get(image) or names.get(image & ~0xFF) or f"module {mid}"
            if not label.startswith("syr"):
                label = "mod " + label
            if bss <= image or bss - image > 0x400000:
                bss = image + 0x100000
            spans.append((image, bss, label))
        m = re.search(r"REL module (\d+) linked in place at 0x([0-9A-F]+)", line)
        if m:
            mid = int(m[1])
            if mid in PLUGINS:
                names[int(m[2], 16)] = PLUGINS[mid]
    return spans


def classify(addr, spans):
    for start, end, label in FIXED:  # main.dol, the code lists and the resident modules (linked at their homes)
        if start <= addr < end:
            return label
    for start, end, label in reversed(spans):  # later links reuse heap addresses
        if start <= addr < end:
            return label
        if start <= addr < end:
            return label
    for start, end, label in FIXED:
        if start <= addr < end:
            return label
    if 0x81050000 <= addr < 0x81070000:
        return "syr trampolines"
    if 0x90000000 <= addr < 0x94000000:
        return "MEM2 heap (other)"
    if 0x80000000 <= addr < 0x81800000:
        return "MEM1 heap (other)"
    return "other"


def group(label):
    if label.startswith("mod ft_"):
        return "fighter modules (in place)"
    if label.startswith("mod st_"):
        return "stage modules (in place)"
    if label.startswith("mod "):
        return "other modules (in place)"
    if label.startswith("res ") or label == "main.dol":
        return "resident/main.dol (patched)"
    if "GCT" in label or label == "gecko handler":
        return "gecko code lists"
    if label.startswith("syr"):
        return "syriinge plugins"
    return "other"


def main():
    spans = regions_from_log(sys.argv[1])
    entries = defaultdict(lambda: [0, 0, 0, 0, 0])
    pcs = defaultdict(int)
    frames = 0
    threads = defaultdict(lambda: [0, 0])
    for path in sys.argv[2:]:
        for line in open(path):
            f = line.split()
            if f[0] == "F":
                frames += int(f[1])
            elif f[0] == "T":
                threads[f[1]][0] += int(f[2])
                threads[f[1]][1] += int(f[3])
            elif f[0] == "E":
                e = entries[int(f[1], 16)]
                e[0] += int(f[2]); e[1] += int(f[3]); e[2] += int(f[4]); e[3] = int(f[5]); e[4] = int(f[6], 16)
            elif f[0] == "P":
                pcs[int(f[1], 16)] += int(f[2])
    frames = max(frames, 1)
    tot_ticks = sum(e[2] for e in entries.values()) or 1
    tot_ins = sum(e[1] for e in entries.values()) or 1
    print(f"frames {frames}  interpreted instructions/frame {tot_ins / frames / 1e6:.3f} M  ticks/frame {tot_ticks / frames / 1e6:.3f} M"
          f"  entries {len(entries)}  calls/frame {sum(e[0] for e in entries.values()) / frames:.0f}")
    for t, (i, k) in sorted(threads.items(), key=lambda x: -x[1][1]):
        print(f"  thread {t:16s} instr/frame {i / frames / 1e6:.3f} M  ticks/frame {k / frames / 1e6:.3f} M")
    by_group = defaultdict(lambda: [0, 0, 0, 0])
    by_label = defaultdict(lambda: [0, 0, 0, 0])
    for addr, e in entries.items():
        label = classify(addr, spans)
        for d in (by_group[group(label) + (" [patched]" if e[3] else "")], by_label[label]):
            d[0] += e[0]; d[1] += e[1]; d[2] += e[2]; d[3] += 1
    print("\n== by entry group (share of interpreter ticks / instructions)")
    for g, d in sorted(by_group.items(), key=lambda x: -x[1][2]):
        print(f"  {100 * d[2] / tot_ticks:6.2f}% ticks {100 * d[1] / tot_ins:6.2f}% instr  {d[3]:5d} entries  "
              f"{d[0] / frames:8.1f} calls/frame  {d[1] / frames / 1e3:8.1f} K instr/frame  {g}")
    print("\n== by entry region")
    for g, d in sorted(by_label.items(), key=lambda x: -x[1][2])[:30]:
        print(f"  {100 * d[2] / tot_ticks:6.2f}% ticks {100 * d[1] / tot_ins:6.2f}% instr  {d[3]:5d} entries  "
              f"{d[0] / frames:8.1f} calls/frame  {g}")
    pc_by = defaultdict(int)
    for pc, n in pcs.items():
        pc_by[classify(pc, spans)] += n
    tot_pc = sum(pc_by.values()) or 1
    print("\n== instructions by PC region (where they execute)")
    for g, n in sorted(pc_by.items(), key=lambda x: -x[1])[:25]:
        print(f"  {100 * n / tot_pc:6.2f}%  {n / frames / 1e3:8.1f} K/frame  {g}")
    print("\n== top entries by ticks")
    for addr, e in sorted(entries.items(), key=lambda x: -x[1][2])[:int(sys.argv[0] and 60)]:
        print(f"  {addr:08X} {100 * e[2] / tot_ticks:6.2f}% {e[0] / frames:8.1f} calls/f {e[1] / max(e[0], 1):7.0f} ins/call "
              f"{'P' if e[3] else ' '} from {e[4]:08X}  {classify(addr, spans)}")


if __name__ == "__main__":
    main()
