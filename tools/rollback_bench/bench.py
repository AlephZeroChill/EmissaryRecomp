#!/usr/bin/env python3
"""Rollback benchmark of EmissaryRecomp against Brawl Online's Dolphin, driven by Brawl Online replays (.brep).

Both engines run Brawl Online's core (brawlonline.net core/): Dolphin through DolphinEngine (the emissary-brawlonline
branch builds Dolphin against core/), EmissaryRecomp through RecompEngine. The session code that decides every save,
load and re-run is the same; only the engine under it differs.

A run plays a replay: the core's playback rebuilds the recorded match from the file (its setup, seeds and countdown),
and where playback would start playing the recorded frames, the benchmark's patch to the core hands over to a
GekkoNet stress session, as gprb_synctest's: every frame rolls back `distance` frames and runs them again (distance 4:
three re-runs a shown frame), each pass with the recorded input of its frame. Distance 0 plays every frame once.
Runs are unthrottled; the timing probe (UnifiedBenchmark.h, brawlonline.net's published one) times each pass and each
snapshot save and load over game frames [first, last).

Every run's per-frame gameplay checksums must equal the replay's own: the file is the ground truth, so both engines
are checked against the game as it was played, and so against each other.

    python3 tools/rollback_bench/bench.py instrument build/rbbench-core     # the probe in the recomp's core checkout
    cmake -DBUILD_DIR=$PWD/build/rbbench -DBRAWL_ONLINE=$PWD/build/rbbench-core -P build.cmake
    python3 tools/rollback_bench/bench.py dolphin-build
    python3 tools/rollback_bench/bench.py prepare
    python3 tools/rollback_bench/bench.py campaign --replay a.brep b.brep --distances 0,2,4,7 --affinity 0-7
    python3 tools/rollback_bench/bench.py report
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import hashlib
import json
import os
import platform
import random
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROBE = HERE / "UnifiedBenchmark.h"
MARK = "UNIFIED_BENCH_INSTRUMENTED v2"
OLD_MARK = "UNIFIED_BENCH_INSTRUMENTED"
DEFAULT_WORK = ROOT / "build/rollback-bench"
DEFAULT_BRAWLONLINE = ROOT.parent / "brawlonline.net"
BRAWL_ONLINE_NETPLAY = Path.home() / ".config/Brawl Online/netplay"  # a Brawl Online install's netplay folder
RECOMP_TOML = (
    "video.resolution_multiplier=1.0;video.frame_interpolation_fps=0;video.vsync=false;video.msaa=1;"
    "video.window_width=1280;video.window_height=720"
)


def command(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args, **kwargs):
    return subprocess.check_output([str(a) for a in args], text=True, **kwargs).strip()


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ------------------------------------------------------------------------------------------------ replays


class Replay:
    """What the benchmark needs from a .brep (core/include/BrawlOnline/Rollback/ReplayFile.h): the session's start
    frame, the region set, and every recorded frame's gameplay checksum. Nothing that names a player."""

    MAGIC = b"\x89BRP\r\n\x1a\n"

    def __init__(self, path: Path):
        self.path = path.resolve()
        data = self.path.read_bytes()
        if data[:8] != self.MAGIC:
            raise ValueError(f"{path}: not a Brawl Online replay")
        self.tag = hashlib.sha256(data).hexdigest()[:12]  # how results name it: never the file's name
        info, setup = {}, {}
        self.checksums: dict[int, int] = {}
        offset = struct.unpack_from("<I", data, 12)[0]
        while offset + 20 <= len(data) and data[offset : offset + 4] != b"BRPT":
            kind, flags, stored, _raw, crc = struct.unpack_from("<4sIIII", data, offset)
            body = data[offset + 20 : offset + 20 + stored]
            if zlib.crc32(body) != crc:
                raise ValueError(f"{path}: damaged chunk")
            if flags & 1:
                body = zlib.decompress(body)
            if kind == b"INFO":
                info = json.loads(body)
            elif kind == b"SETP":
                setup = json.loads(body)
            elif kind == b"FRMS":
                first, count, ports = struct.unpack_from("<IHB", body, 0)
                at = 8 + bin(ports & 0xF).count("1") * count * 0x40
                for i in range(count):
                    self.checksums[first + i] = struct.unpack_from("<I", body, at + 4 * i)[0]
            offset += 20 + stored
        self.start_frame = int(setup.get("start_frame", 120))
        self.region_set = info.get("session", {}).get("regionSet", "gp-v29")
        self.stage = info.get("stageId")
        self.frames = len(self.checksums)

    def describe(self) -> dict:
        return {
            "tag": self.tag,
            "sha256": sha256(self.path),
            "stage": self.stage,
            "frames": self.frames,
            "start_frame": self.start_frame,
            "region_set": self.region_set,
        }


# ------------------------------------------------------------------------------------------------ instrumentation

REPLAY_HELPER = r"""
// Benchmark-only (EmissaryRecomp tools/rollback_bench, UNIFIED_BENCH_INSTRUMENTED v2):
// PPR_UNIFIED_BENCH_REPLAY=<distance> makes a replay's playback a rollback sync test once playback has rebuilt the
// recorded match. Where playback would start playing its frames, a GekkoNet stress session takes over as
// gprb_synctest's does, rolling back <distance> frames on every frame, and every pass takes its input from the replay
// by session frame, as playback does. PPR_UNIFIED_BENCH_REPLAY_REFERENCE=1 runs every frame once instead;
// PPR_UNIFIED_BENCH_REGION_SET names the region set.
static std::shared_ptr<const Replay::File> s_bench_replay;
static u32 s_bench_replay_ports = 0;

static void UnifiedBenchReplayStart()
{
  const char* distance = std::getenv("PPR_UNIFIED_BENCH_REPLAY");
  if (!distance || s.mode != Mode::Playback || !s.pb.file)
    return;
  s_bench_replay = s.pb.file;
  s_bench_replay_ports = s.pb.ports;
  SyncTestOptions options;
  options.distance = std::clamp(std::atoi(distance), 1, static_cast<int>(MAX_ROLLBACK_FRAMES));
  if (const char* region_set = std::getenv("PPR_UNIFIED_BENCH_REGION_SET"); region_set && *region_set)
    options.region_set = region_set;
  options.hash_regions = false;
  options.start_frame = s.pb.start_frame;
  options.ports = s.pb.ports;
  options.no_rollback = std::getenv("PPR_UNIFIED_BENCH_REPLAY_REFERENCE") != nullptr;
  s.mode = Mode::SyncTest;
  s.st_opts = options;
  WARN_LOG_FMT(BRAWLBACK, "unified bench: the replay goes on as a sync test, distance {}{}", options.distance,
               options.no_rollback ? ", no rollback" : "");
}

static const PadSlots* UnifiedBenchReplayPads(s64 frame)
{
  if (!s_bench_replay || s.mode != Mode::SyncTest)
    return nullptr;
  const auto& file = *s_bench_replay;
  const s64 index = frame - static_cast<s64>(file.first_frame);
  if (index < 0 || index >= static_cast<s64>(file.pads.size()))
    return nullptr;
  static PadSlots slots;
  slots = file.pads[static_cast<size_t>(index)];
  for (int port = 0; port < MAX_LOBBY_PLAYERS; ++port)
  {
    if ((s_bench_replay_ports >> port) & 1)
    {
      PeerData::SanitizePad(slots.data() + port * INPUT_SIZE);
    }
    else
    {
      std::fill(slots.begin() + port * INPUT_SIZE, slots.begin() + (port + 1) * INPUT_SIZE, u8(0));
      slots[port * INPUT_SIZE + 0x38] = 0xFF;  // gfPadError NO_CONTROLLER
    }
  }
  return &slots;
}

"""


def replace_once(text: str, old: str, new: str, what: str) -> str:
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"instrument: {what}: expected one match, found {n}")
    return text.replace(old, new)


def instrument(core_root: Path) -> bool:
    """Puts the probe and the replay hand-over into <core_root>/core/src/Rollback/GameplaySession.cpp (a git
    checkout: an older instrumentation is restored from git first). Returns whether it changed anything."""
    relative = "core/src/Rollback/GameplaySession.cpp"
    session = core_root / relative
    rollback = session.parent
    text = session.read_text()
    shutil.copyfile(PROBE, rollback / "UnifiedBenchmark.h")
    if MARK in text:
        return False
    if OLD_MARK in text:
        command(["git", "-C", core_root, "checkout", "--", relative])
        text = session.read_text()
    text = f'#include "UnifiedBenchmark.h"  // {MARK} (EmissaryRecomp tools/rollback_bench)\n' + text
    # The probe: each pass's hand-over to the game, and the engine's snapshot calls.
    text = replace_once(
        text,
        "bool RunFrame(const Core::CPUThreadGuard& guard)\n{\n  CostPassEnd();\n",
        REPLAY_HELPER + "bool RunFrame(const Core::CPUThreadGuard& guard)\n{\n"
        "  UnifiedRollbackBench::EndPass();\n  CostPassEnd();\n",
        "RunFrame entry",
    )
    text = replace_once(
        text,
        '  SetWhere("guest");\n',
        '  SetWhere("guest");\n  UnifiedRollbackBench::BeginPass(s.resim_pass,\n'
        "      static_cast<long long>(g.U32(Addr::GAME_FRAME + 4).value_or(0)) + 1);\n",
        "RunFrame's hand-over to the game",
    )
    engine = "BrawlOnline::GetEngine()"
    for pattern, replacement, count in [
        (r"BrawlOnline::GetEngine\(\)\.SaveFrame\(", f"UnifiedRollbackBench::TimedSave({engine}, ", 2),
        (r"\brbm\.SaveFrame\(", "UnifiedRollbackBench::TimedSave(rbm, ", 2),
        (r"BrawlOnline::GetEngine\(\)\.LoadFrame\(", f"UnifiedRollbackBench::TimedLoad({engine}, ", 1),
    ]:
        text, n = re.subn(pattern, replacement, text)
        if n != count:
            raise RuntimeError(f"instrument: {pattern}: expected {count}, found {n}")
    if re.search(r"\b(?:rbm|ct)\.LoadFrame\(", text):
        raise RuntimeError("instrument: an untimed load")
    # The replay: the hand-over where playback would start its frames, and each pass's recorded input.
    text = replace_once(
        text,
        "    if (s.mode == Mode::Playback)\n    {\n      // The recorded frames from here:",
        "    UnifiedBenchReplayStart();\n"
        "    if (s.mode == Mode::Playback)\n    {\n      // The recorded frames from here:",
        "playback's start",
    )
    recorded = "UnifiedBenchReplayPads(static_cast<s64>(s.ops.adv_frame[i]))"
    text = replace_once(
        text,
        "  PadSlots slots = s.ops.slots[i];\n",
        f"  PadSlots slots = s.ops.slots[i];\n  if (const PadSlots* recorded = {recorded})\n    slots = *recorded;\n",
        "each pass's input",
    )
    session.write_text(text)
    return True


def cmd_instrument(args):
    changed = instrument(args.core.resolve())
    print(("Instrumented " if changed else "Already instrumented ") + str(args.core))


# ------------------------------------------------------------------------------------------------ Dolphin build


def link_externals(brawlonline: Path, source: Path, sha: str):
    """The worktree's dolphin/Externals submodules, symlinked to the main checkout's (rollback_benchmark.py)."""
    deps = []
    for line in output(["git", "-C", brawlonline, "ls-tree", "-r", sha]).splitlines():
        if not line.startswith("160000"):
            continue
        meta, relative = line.split("\t", 1)
        if not relative.startswith("dolphin/Externals/"):
            continue
        original, dest = brawlonline / relative, source / relative
        if not original.is_dir() or not any(original.iterdir()):
            raise RuntimeError(f"submodule missing in {brawlonline}: {relative}")
        if not dest.is_symlink():
            if dest.is_dir():
                dest.rmdir()
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.symlink_to(original, target_is_directory=True)
        deps.append({"path": relative, "gitlink_sha": meta.split()[2]})
    return deps


def cmd_dolphin_build(args):
    work, brawlonline = args.work.resolve(), args.brawlonline.resolve()
    source = work / "dolphin-src"
    sha = output(["git", "-C", brawlonline, "rev-parse", args.ref + "^{commit}"])
    if not source.exists():
        work.mkdir(parents=True, exist_ok=True)
        command(["git", "-C", brawlonline, "worktree", "add", "--detach", source, sha])
    head = output(["git", "-C", source, "rev-parse", "HEAD"])
    if head != sha:
        raise RuntimeError(f"{source} is at {head}, not {args.ref} ({sha}); remove the worktree to rebuild another")
    deps = link_externals(brawlonline, source, sha)
    instrument(source)
    patch = output(["git", "-C", source, "diff", "--", "core"])
    dest = work / "dolphin"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "instrumentation.patch").write_text(patch + "\n")
    flags = [
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-DENABLE_QT=OFF",
        "-DENABLE_NOGUI=ON",
        "-DENABLE_TESTS=OFF",
        "-DENABLE_LTO=OFF",
        "-DENABLE_LLVM=OFF",
        "-DENABLE_ANALYTICS=OFF",
        "-DENABLE_AUTOUPDATE=OFF",
        "-DUSE_DISCORD_PRESENCE=OFF",
        "-DENABLE_CCACHE=OFF",
        "-DCMAKE_C_COMPILER=/usr/bin/cc",
        "-DCMAKE_CXX_COMPILER=/usr/bin/c++",
        "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
    ]
    env = dict(os.environ, CCACHE_BASEDIR=str(source), CCACHE_NOHASHDIR="true")
    started = time.time()
    compile_dir = work / "dolphin-compile"
    command(["cmake", "-S", source / "dolphin", "-B", compile_dir, *flags], env=env)
    command(["cmake", "--build", compile_dir, "--target", "dolphin-nogui", "-j", args.jobs], env=env)
    binary = dest / "dolphin-emu-nogui"
    shutil.copy2(compile_dir / "Binaries/dolphin-emu-nogui", binary)
    shutil.copytree(source / "dolphin/Data/Sys", dest / "Sys", dirs_exist_ok=True)
    write_json(
        dest / "manifest.json",
        {
            "ref": args.ref,
            "source_sha": sha,
            "binary_sha256": sha256(binary),
            "probe_sha256": sha256(PROBE),
            "cmake_flags": flags,
            "dependencies": deps,
            "build_seconds": round(time.time() - started, 1),
            "built_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        },
    )
    print(f"Built {binary}")


# ------------------------------------------------------------------------------------------------ inputs


def cmd_prepare(args):
    """What both engines play from, as Brawl Online plays its games: the harness's user folder with Brawl Online's
    online card (P+ and the plugin) and the netplay save in its NAND, and the recomp's UserData as Brawl Online's
    launcher sets it up (launcher/src/dolphin/emissary/launch.ts: the card's netplay codes and gc.txt in Codes/, the
    same NAND). Dolphin boots the netplay launcher, which loads the same codes."""
    work = args.work.resolve()
    template = work / "template"
    if template.exists():
        shutil.rmtree(template)
    command(["cp", "-a", "--reflink=auto", args.source_template.resolve(), template])
    (template / "Wii/sd.raw").unlink(missing_ok=True)
    command(["cp", "--reflink=auto", args.card.resolve(), template / "Wii/sd.raw"])
    shutil.copytree(args.netplay_save.resolve() / "title", template / "Wii/title", dirs_exist_ok=True)
    card = template / "Wii/sd.raw"
    user = args.recomp.resolve() / "UserData"
    codes = user / "Codes"
    codes.mkdir(parents=True, exist_ok=True)
    command(["mcopy", "-o", "-n", "-i", card, "::/Project+/NETPLAY.GCT", codes / "RSBE01.gct"])
    command(["mcopy", "-o", "-n", "-i", card, "::/Project+/gc.txt", codes / "gc.txt"])
    nand = user / "NAND"
    if nand.exists():
        shutil.rmtree(nand)
    shutil.copytree(template / "Wii", nand, ignore=shutil.ignore_patterns("sd.raw"))
    write_json(
        work / "content.json",
        {
            "card_sha256": sha256(card),
            "codes_sha256": sha256(codes / "RSBE01.gct"),
            "gc_txt": (codes / "gc.txt").read_text(),
        },
    )
    print(f"Prepared {template}, {codes}, {nand}")


# ------------------------------------------------------------------------------------------------ runs


def harness(work: Path):
    """brawlonline.net's harness from the Dolphin worktree (its ppharness has RecompInstance)."""
    source = work / "dolphin-src"
    for p in (source / "harness/tools", source / "harness"):
        if str(p) not in sys.path:
            sys.path.insert(0, str(p))
    import gameplay_rollback as G
    from ppharness import brawl as B
    from ppharness.instance import InstanceConfig
    from ppharness.recomp import RecompInstance

    return G, B, InstanceConfig, RecompInstance


def recomp_manifest(recomp: Path) -> dict:
    exe = recomp / "WiiCompiled"
    core = Path(re.search(r"BRAWL_ONLINE_ROOT:\w+=(.*)", (recomp / "CMakeCache.txt").read_text())[1])
    stamp = core / ".brawl-online-commit"
    session = core / "core/src/Rollback/GameplaySession.cpp"
    return {
        "binary_sha256": sha256(exe),
        "core_commit": stamp.read_text().split()[0] if stamp.exists() else None,
        "core_instrumented": MARK in session.read_text(),
        "probe_sha256": sha256(PROBE),
        "recomp_commit": output(["git", "-C", ROOT, "rev-parse", "HEAD"]),
        "recomp_dirty": bool(output(["git", "-C", ROOT, "status", "--porcelain", "--", "wiicompiled"])),
        "toml": RECOMP_TOML,
    }


def set_environment(work: Path, args, engine: str, replay: Replay, distance: int, raw: Path):
    os.environ.update(
        {
            "PPHARNESS_DOLPHIN_DIR": str(work / "dolphin"),
            "PPHARNESS_DOLPHIN": str(work / "dolphin/dolphin-emu-nogui"),
            "PPHARNESS_TEMPLATE": str(work / "template"),
            "PPHARNESS_INSTANCES": str(work / "instances"),
            "PPR_GPRB_NO_FRAME_COST": "1",
            "PPR_UNIFIED_BENCH_OUTPUT": str(raw),
            "PPR_UNIFIED_BENCH_FIRST": str(args.first),
            "PPR_UNIFIED_BENCH_LAST": str(args.last),
            "PPR_UNIFIED_BENCH_REPLAY": str(max(distance, 2)),
            "PPR_UNIFIED_BENCH_REGION_SET": replay.region_set,
        }
    )
    if distance == 0:
        os.environ["PPR_UNIFIED_BENCH_REPLAY_REFERENCE"] = "1"
    else:
        os.environ.pop("PPR_UNIFIED_BENCH_REPLAY_REFERENCE", None)
    if engine == "recomp":
        # SDL's offscreen video driver: no window on anyone's screen (Vulkan through VK_EXT_headless_surface), as
        # Dolphin's headless platform renders without presenting.
        os.environ.update(
            {
                "PPHARNESS_RECOMP": str(args.recomp.resolve()),
                "PPHARNESS_RECOMP_TOML": RECOMP_TOML,
                "BRAWL_UNTHROTTLED": "1",
                "SDL_VIDEO_DRIVER": "offscreen",
            }
        )
    else:
        for name in ("PPHARNESS_RECOMP", "BRAWL_UNTHROTTLED", "SDL_VIDEO_DRIVER"):
            os.environ.pop(name, None)


def make(engine: str, name: str, args, G, InstanceConfig, RecompInstance):
    config = [
        "Dolphin.Core.EmulationSpeed=0",
        "Logger.Options.WriteToFile=False",
        "Logger.Options.WriteToConsole=False",
    ]
    if engine == "dolphin":
        inst = G.make_instance(name, cpu_thread=True, video=args.video, config=config, keep=True)
        inst.boot = "netplay"  # the netplay launcher: the card's NETPLAY.GCT, as Brawl Online's netplay Dolphin
        if args.dolphin_display == "gamescope":
            # Dolphin's dual-core Vulkan renderer with `-p headless` crashes in the GPU driver while binding a draw's
            # descriptors, on some replays every time, unmodified builds too. In gamescope's headless backend it renders
            # and presents to an X11 window that is on no screen, as the recomp presents to its offscreen surface.
            inst.command_prefix = [
                "gamescope",
                "--backend",
                "headless",
                "-W",
                "1280",
                "-H",
                "720",
                "--",
                *inst.command_prefix,
            ]
            inst.platform = "x11"
        return inst
    inst = RecompInstance(
        name,
        config=InstanceConfig(cpu_thread=True, config_args=config, standard_controllers=(0, 1)),
        keep=True,
        connect_timeout=180,
    )
    inst.create()
    return inst


def compare_checksums(replay: Replay, rows: list, last_session_frame: int) -> dict:
    """The run's final checksum of each session frame against the replay's."""
    compared, mismatches, first = 0, 0, None
    for row in rows:
        frame, checksum = int(row[0]), int(row[1])
        if frame > last_session_frame or frame not in replay.checksums:
            continue
        compared += 1
        if checksum != replay.checksums[frame]:
            mismatches += 1
            first = frame if first is None else min(first, frame)
    return {"compared": compared, "mismatches": mismatches, "first_mismatch": first}


def quantile(ordered: list[float], q: float) -> float:
    x = (len(ordered) - 1) * q
    i = int(x)
    return ordered[i] + (ordered[min(i + 1, len(ordered) - 1)] - ordered[i]) * (x - i)


def sample_summary(path: Path) -> dict:
    samples: dict[str, list[float]] = {}
    with path.open() as f:
        for row in csv.DictReader(f):
            samples.setdefault(row["kind"], []).append(int(row["ns"]) / 1e6)
    out = {}
    for kind, values in samples.items():
        ordered = sorted(values)
        out[kind] = {
            "n": len(values),
            "mean_ms": statistics.fmean(values),
            "median_ms": quantile(ordered, 0.5),
            "p95_ms": quantile(ordered, 0.95),
            "p99_ms": quantile(ordered, 0.99),
            "max_ms": max(values),
        }
    return out


def run_one(args, engine: str, replay: Replay, distance: int, round_number: int) -> dict:
    work = args.work.resolve()
    G, B, InstanceConfig, RecompInstance = harness(work)
    name = f"{engine}-{replay.tag}-d{distance}-r{round_number}"
    dest = work / "runs" / args.campaign / name
    dest.mkdir(parents=True, exist_ok=True)
    result_path = dest / "result.json"
    if result_path.exists() and json.loads(result_path.read_text()).get("valid") and not args.force:
        return json.loads(result_path.read_text())
    if replay.start_frame + replay.frames < args.last + 30:
        raise RuntimeError(
            f"replay {replay.tag} ends at game frame ~{replay.start_frame + replay.frames}, "
            f"before the window's end ({args.last})"
        )
    raw = dest / "samples.csv"
    raw.unlink(missing_ok=True)
    set_environment(work, args, engine, replay, distance, raw)
    build = (
        json.loads((work / "dolphin/manifest.json").read_text())
        if engine == "dolphin"
        else recomp_manifest(args.recomp.resolve())
    )
    result = {
        "engine": engine,
        "replay": replay.describe(),
        "distance": distance,
        "round": round_number,
        "campaign": args.campaign,
        "build": build,
        "first_game_frame": args.first,
        "last_game_frame_exclusive": args.last,
        "video": f"{args.video} ({args.dolphin_display})" if engine == "dolphin" else "aurora offscreen",
        "started_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "load_average_before": os.getloadavg(),
        "valid": False,
    }
    begin = time.monotonic()
    inst = None
    try:
        inst = make(engine, f"xbench-{name}", args, G, InstanceConfig, RecompInstance)
        inst.launch()
        inst.connect()
        c = inst.client
        c.wait_state("running", timeout=180)
        for port in (0, 1):
            c.pad_set(port)
        while B.read_scene(c.read_mem).name != "muMenuMain":  # Brawl Online's card boots to the main menu
            if time.monotonic() - begin > 180:
                raise RuntimeError("the game never reached its main menu")
            B.tap(c, 0, ["A"], hold=8, release=60)
        c.call("replay_play", path=str(replay.path), play_anyway=False)
        while not raw.exists():
            if time.monotonic() - begin > args.timeout:
                raise RuntimeError("timed out before the measurement window completed")
            if not inst.is_running():
                raise GameExited(f"the game exited before completing the window (code {inst.process.poll()})")
            time.sleep(0.25)
        status = c.call("gprb_status")
        checksums = c.call("gprb_checksums", since=0)
        write_json(dest / "gprb_status.json", status)
        write_json(dest / "checksums.json", checksums)
        result["status"] = {
            k: status.get(k)
            for k in [
                "mode",
                "phase",
                "current_frame",
                "rollbacks",
                "frames_resimulated",
                "desyncs_detected",
                "desync_log",
                "region_bytes",
                "region_ranges",
                "save_count",
                "load_count",
                "error",
            ]
        }
        # Frames well inside what has run: the game goes on, unthrottled, while this reads.
        last_session_frame = int(checksums.get("current_frame", 0)) - 2 * max(distance, 1) - 10
        result["replay_checksums"] = compare_checksums(replay, checksums.get("rows", []), last_session_frame)
        result["metrics"] = sample_summary(raw)
        result["sample_sha256"] = sha256(raw)
        expected = args.last - args.first
        metrics, check = result["metrics"], result["replay_checksums"]
        result["valid"] = (
            status.get("mode") == "synctest"
            and metrics.get("shown_frame", {}).get("n") == expected
            and metrics.get("update", {}).get("n") == expected
            and check["mismatches"] == 0
            and check["compared"] >= args.last - replay.start_frame - 2 * max(distance, 1) - 30
            and
            # Without rollback GekkoNet's stress session flags every frame it expected to re-run: the
            # replay's checksums are what count there.
            (distance == 0 or not status.get("desyncs_detected"))
        )
        if not result["valid"]:
            raise RuntimeError("incomplete window, a desync, or checksums that differ from the replay's")
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        result["elapsed_s"] = time.monotonic() - begin
        result["load_average_after"] = os.getloadavg()
        if inst is not None:
            for src, name_ in [
                (inst.user_dir / "harness-stderr.txt", "stderr.txt"),
                (inst.user_dir / "harness-stdout.txt", "stdout.txt"),
                (inst.user_dir / "Logs/dolphin.log", "dolphin.log"),
            ]:
                if src and src.is_file():
                    shutil.copyfile(src, dest / name_)
            with contextlib.suppress(Exception):
                if inst.is_running():
                    inst.stop()
            with contextlib.suppress(Exception):
                if inst.is_running():
                    inst.kill()
            with contextlib.suppress(Exception):
                inst.keep = False
                inst.cleanup(True)
        write_json(result_path, result)
        os.environ.pop("PPR_UNIFIED_BENCH_OUTPUT", None)
    print(f"{name}: " + ", ".join(f"{k} {v['mean_ms']:.3f} ms" for k, v in result["metrics"].items()), flush=True)
    return result


class GameExited(RuntimeError):
    pass


def run_with_retries(args, engine: str, replay: Replay, distance: int, round_number: int) -> dict:
    """A run whose game process dies is run again, up to --retries times: Dolphin's dual-core Vulkan renderer crashes
    now and then in the GPU driver, unmodified builds too. A run only counts when it matches
    the replay's checksums, so a retry cannot pass off a wrong result."""
    for attempt in range(args.retries + 1):
        try:
            result = run_one(args, engine, replay, distance, round_number)
            if attempt:
                name = f"{engine}-{replay.tag}-d{distance}-r{round_number}"
                result["attempts"] = attempt + 1
                write_json(args.work.resolve() / "runs" / args.campaign / name / "result.json", result)
            return result
        except GameExited as exc:
            print(
                f"{engine} {replay.tag} d{distance} r{round_number}: {exc}; attempt {attempt + 1} of "
                f"{args.retries + 1}",
                file=sys.stderr,
                flush=True,
            )
            if attempt == args.retries:
                raise


def replays_from(args) -> list[Replay]:
    paths = []
    for item in args.replay:
        p = Path(item)
        paths += sorted(p.rglob("*.brep")) if p.is_dir() else [p]
    if not paths:
        raise SystemExit("no replays given (--replay FILE_OR_FOLDER ...)")
    return [Replay(p) for p in paths]


def cmd_run(args):
    for replay in replays_from(args):
        run_with_retries(args, args.engine, replay, args.distance, args.round)


def cmd_campaign(args):
    work = args.work.resolve()
    if args.affinity:
        cpus = set()
        for part in args.affinity.split(","):
            lo, _, hi = part.partition("-")
            cpus.update(range(int(lo), int(hi or lo) + 1))
        os.sched_setaffinity(0, cpus)
    else:
        print(
            "warning: no --affinity: unpinned runs share CPUs with everything else on the PC and vary much more "
            "from run to run; compare only campaigns pinned to the same CPUs",
            file=sys.stderr,
            flush=True,
        )
    replays = replays_from(args)
    engines = args.engines.split(",")
    distances = [int(x) for x in args.distances.split(",")]
    # What the campaign ran, not what it ran on: no CPU, GPU, OS or path details in its records.
    write_json(
        work / f"campaign-{args.campaign}.json",
        {
            "affinity": args.affinity or None,
            "python": platform.python_version(),
            "probe_sha256": sha256(PROBE),
            "runner_sha256": sha256(Path(__file__)),
            "first": args.first,
            "last": args.last,
            "rounds": args.rounds,
            "distances": distances,
            "engines": engines,
            "replays": [r.describe() for r in replays],
            "order_seed": args.seed,
        },
    )
    rng = random.Random(args.seed)
    for round_number in range(args.rounds):
        block = [(e, r, d) for e in engines for r in replays for d in distances]
        rng.shuffle(block)
        for engine, replay, distance in block:
            run_with_retries(args, engine, replay, distance, round_number)


# ------------------------------------------------------------------------------------------------ report


def bootstrap(values, seed):
    rng = random.Random(seed)
    estimates = sorted(statistics.fmean(rng.choices(values, k=len(values))) for _ in range(10000))
    return [estimates[249], estimates[9749]]


KINDS = ["save", "load", "shown_frame", "loaded_frame", "resim_frame", "update"]


def summarize(group: list, seed: str) -> dict:
    metrics = {}
    for kind in KINDS:
        values = [r["metrics"][kind]["mean_ms"] for r in group if kind in r["metrics"]]
        if values:
            metrics[kind] = {
                "run_means_ms": values,
                "mean_ms": statistics.fmean(values),
                "ci95_ms": bootstrap(values, f"{seed}-{kind}"),
                "mean_run_p99_ms": statistics.fmean(
                    r["metrics"][kind]["p99_ms"] for r in group if kind in r["metrics"]
                ),
            }
    return metrics


def cmd_report(args):
    work = args.work.resolve()
    results = [json.loads(p.read_text()) for p in (work / "runs" / args.campaign).glob("*/result.json")]
    valid = [r for r in results if r.get("valid")]
    invalid = [r for r in results if not r.get("valid")]
    summaries = []
    for engine in sorted({r["engine"] for r in valid}):
        for distance in sorted({r["distance"] for r in valid if r["engine"] == engine}):
            group = [r for r in valid if r["engine"] == engine and r["distance"] == distance]
            per_replay = {}
            for tag in sorted({r["replay"]["tag"] for r in group}):
                runs = [r for r in group if r["replay"]["tag"] == tag]
                per_replay[tag] = summarize(runs, f"{engine}-{distance}-{tag}")
            summaries.append(
                {
                    "engine": engine,
                    "distance": distance,
                    "runs": len(group),
                    "replays": len(per_replay),
                    "metrics": summarize(group, f"{engine}-{distance}"),
                    "per_replay": per_replay,
                }
            )
    write_json(
        work / f"report-{args.campaign}.json",
        {"campaign": args.campaign, "summaries": summaries, "invalid_runs": [r.get("error") for r in invalid]},
    )
    print(f"{len(valid)} valid runs, {len(invalid)} invalid; every valid run matched its replay's checksums")
    print(f"{'engine':8} {'d':>2} {'runs':>4} " + " ".join(f"{k:>13}" for k in KINDS))
    for s in summaries:
        print(
            f"{s['engine']:8} {s['distance']:>2} {s['runs']:>4} "
            + " ".join(f"{s['metrics'][k]['mean_ms']:13.3f}" if k in s["metrics"] else f"{'-':>13}" for k in KINDS)
        )
    by = {(s["engine"], s["distance"]): s for s in summaries}
    for (engine, distance), s in sorted(by.items()):
        if engine == "recomp" and ("dolphin", distance) in by:
            d = by[("dolphin", distance)]
            print(
                f"recomp / dolphin at distance {distance}: "
                + ", ".join(
                    f"{k} {s['metrics'][k]['mean_ms'] / d['metrics'][k]['mean_ms']:.2f}x"
                    for k in KINDS
                    if k in s["metrics"] and k in d["metrics"]
                )
            )


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--work", type=Path, default=DEFAULT_WORK)
    p.add_argument("--brawlonline", type=Path, default=DEFAULT_BRAWLONLINE)
    sub = p.add_subparsers(dest="command", required=True)
    i = sub.add_parser("instrument", help="put the probe and the replay hand-over into a core checkout")
    i.add_argument("core", type=Path)
    b = sub.add_parser("dolphin-build")
    b.add_argument("--ref", default="origin/emissary-brawlonline")
    b.add_argument("--jobs", type=int, default=16)
    pr = sub.add_parser("prepare", help="set up both engines from a Brawl Online install")
    pr.add_argument("--recomp", type=Path, default=ROOT / "build/rbbench/native")
    pr.add_argument(
        "--card",
        type=Path,
        default=BRAWL_ONLINE_NETPLAY / "pponline-sd/sd.raw",
        help="Brawl Online's online card (P+ with the plugin)",
    )
    pr.add_argument("--netplay-save", type=Path, default=BRAWL_ONLINE_NETPLAY / "pplus/NetplaySave")
    pr.add_argument(
        "--source-template",
        type=Path,
        default=DEFAULT_BRAWLONLINE / "run/template-user",
        help="brawlonline.net's harness user folder",
    )
    for name in ("run", "campaign"):
        r = sub.add_parser(name)
        r.add_argument("--replay", nargs="+", required=True, help=".brep files, or folders of them")
        r.add_argument("--recomp", type=Path, default=ROOT / "build/rbbench/native")
        r.add_argument("--first", type=int, default=600, help="first game frame measured")
        r.add_argument("--last", type=int, default=2400, help="game frame the window ends before")
        r.add_argument("--timeout", type=float, default=600)
        r.add_argument("--video", default="Vulkan", choices=["Null", "Vulkan", "OGL"], help="Dolphin's video backend")
        r.add_argument(
            "--dolphin-display",
            default="gamescope",
            choices=["gamescope", "headless"],
            help="where Dolphin renders: a headless gamescope (default) or Dolphin's own headless platform",
        )
        r.add_argument("--campaign", default="main")
        r.add_argument("--force", action="store_true", help="run again even if a valid result is there")
        r.add_argument("--retries", type=int, default=2, help="runs again a run whose game process died")
        if name == "run":
            r.add_argument("--engine", choices=["dolphin", "recomp"], required=True)
            r.add_argument("--distance", type=int, default=4, help="frames rolled back every frame; 0: no rollback")
            r.add_argument("--round", type=int, default=0)
        else:
            r.add_argument("--engines", default="dolphin,recomp")
            r.add_argument("--distances", default="4")
            r.add_argument("--rounds", type=int, default=5)
            r.add_argument("--seed", default="crossengine-rollback-order-v1")
            r.add_argument(
                "--affinity",
                default="",
                help="CPUs to pin the runs to (e.g. 8-15,24-31); the same set for both engines",
            )
    rp = sub.add_parser("report")
    rp.add_argument("--campaign", default="main")
    args = p.parse_args()
    {
        "instrument": cmd_instrument,
        "dolphin-build": cmd_dolphin_build,
        "prepare": cmd_prepare,
        "run": cmd_run,
        "campaign": cmd_campaign,
        "report": cmd_report,
    }[args.command](args)


if __name__ == "__main__":
    main()
