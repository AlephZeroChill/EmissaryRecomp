# Performance: where a Project+ frame goes (2026-11-26)

Brawl Online board card 899: "do a thorough investigation of emissary's performance ... profile emissary, find what's
most expensive". Measured on Sarah's PC (Ryzen 7 9800X3D, RTX 4070, Windows 11), on `brawlonline/windows-build`
66a8cd0 with Brawl Online's core of brawlonline.net main eaec2021f, playing Brawl Online replays (`.brep`, recorded on
Dolphin) through the core's playback. Every run's per-frame checksums are checked against the recording.

## How it was measured

- **Workload.** Replay 004 of the 50-replay corpus (`E:\emissary\core\replays`; P+ Ganondorf vs Game & Watch on Green
  Hill, 6,558 frames), unthrottled: `BRAWL_UNTHROTTLED=1` (kalomaze PR #3's switch in `det_clock.cpp`) lets the
  deterministic clock run as fast as the game does, so wall time per frame is the cost. Rollback cost: PR #3's
  `tools/rollback_bench` probe in the core (`bench.py instrument`), where playback turns into a sync test that rolls
  back N frames every frame (`PPR_UNIFIED_BENCH_REPLAY=4`) and times each pass, save and load.
- **CPU per frame.** The game thread's `QueryThreadCycleTime` over the match, divided by the frames played (Windows'
  per-thread CPU times are tick-sampled and read 5x low for this thread).
- **Where.** The in-process sampler (`BRAWL_ONLINE_PROFILE=1`, 1 kHz on the game thread, x64 unwind), resolved with
  `WiiCompiled.map`; since bb761c0 it also names who made each call into ntdll/ucrtbase/libc++. Interpreter
  attribution: an `MKW_INTERP_PROFILE` build with `BRAWL_INTERP_PROFILE=<prefix>` and
  `BRAWL_INTERP_PROFILE_FRAMES=1200`, read with `projects/brawl/tools/interp_report.py`. Per-frame patch counters:
  `BRAWL_PATCH_STATS=<file>`. Write traps: `BRAWL_TRAP_STATS=1` (a510744).
- **Noise.** Other sessions build and test on the same PC. Under that load the same build's game thread went from 2.5
  to 6.7 ms a frame, and even CPU cycles inflate (shared caches, SMT). Timings below were taken with every run waiting
  for the machine to be under 15% CPU, variants interleaved; profile shares are stable under load.

## Where the game thread's time went (base build)

Replay 004, unthrottled playback, base build (flat memory, no code variants: what releases ship). Samples by what the
leaf function is:

| | share |
|---|---|
| translated game code (`func_*`) | 26% |
| interpreter (`PpcInterpreter::Run`, handlers) | 26% |
| `GuestCodePatch::Sync` (self 8%, with its memcmp and hashing ~20-24%) | 20-24% |
| ntdll/ucrtbase: write traps (NtProtectVirtualMemory, exception dispatch), waits for the GX thread and the audio mix worker, log writes | ~15% |
| GX HLE / aurora on the game thread | 4% |
| indirect call dispatch | 4% |

Per frame (BRAWL_PATCH_STATS, mid-match medians): 1,700 patched calls, all of them in the interpreter (0 code-variant
calls); 1,742 page compares; 90 write traps; 343K interpreted guest instructions.

### 1. The interpreter is Project+'s patches to the disc's code, not its fighters

The interpreter profile of the same match: **98% of interpreter time is in translated functions that P+'s codes
patched** (main.dol 64%, sora_melee 35%), entered 1,712 times a frame. P+'s own modules linked in place (fighters,
stages) are under 1.5%: most fighter code lives in sora_melee and main.dol, which are translated. Top entries:
0x8002E578 (22.6%, once a frame, 37,888 instructions), 0x8002DC74 (11.8%, 1,060 calls a frame of 15 instructions),
0x8078249C (8.0%), 0x8001789C (6.3%), 0x801933CC (5.1%).

The runtime already has the fix: **code variants** (`-DCODE_VARIANTS=<spec>`, translator `--code-variants`,
`projects/brawl/tools/gen_variant_spec.py`), translations of functions as a mod patches them, run while guest memory
holds exactly those words (else the interpreter, so a different P+ version is only slower). Releases don't build with a
spec. One made from replay 004 (dumps at the first scMelee frame plus the run's log of patched functions): 1,201
functions asked, 1,162 translated. With it:

- interpreted guest instructions per frame 343K -> 13K (-96%); interpreter calls 1,714 -> 9 a frame. What is left is
  our own PPOnline plugin's per-frame code (8.9K instructions) and fighter/stage modules (2.7K).
- on replays 010, 020 and 030 (other characters and stages) the same spec leaves 8-16 interpreter calls a frame.
- the corpus: see "Checks" below.

Making the spec (what was done here; any Brawl Online replay works, it only has to reach a match):

1. A build without variants (its `source/wiicompiled/generated` has the guard table the spec is made against).
2. Play a replay through the core's playback (the launcher's comm file, `BRAWL_ONLINE_REPLAY_COMM`) with
   `BRAWL_PAD_SCRIPT=nosuchscene+99999:A` (the scene hook only runs with a pad script; this one never presses) and
   `BRAWL_DUMP_ON_SCENE=scMelee:80001800-80003000,80550000-80566000,80566000-80580000,80004000-80406500,80673F00-80B85000,81050000-81070000:<dir>/pp`.
3. `python wiicompiled/projects/brawl/tools/gen_variant_spec.py <build>/source/wiicompiled <dir>/pp <spec dir>
   --patched-log <that run's stderr>`: 2,717 patched words in 892 functions, 229 inlining callers, 27 continuations.
4. Build with `-DCODE_VARIANTS=<spec dir>/spec.txt` (build.cmake). The spec dir holds `spec.txt` (the patched words,
   ~70 KB) and dumps of P+'s two code lists and Syriinge's heap (~330 KB), which the translator reads for the hook
   bodies. They are Project+'s codes and our plugin's trampolines as they sit in memory, so they belong with the game
   data CI decrypts, not in this public repository.

A spec matches one P+ card and plugin: after either changes, the variants whose words differ fall back to the
interpreter (checked at run time, so never wrong, only slower) until a spec made from the new card is built in.

### 2. Sync after every patched call

See a510744. Fixed: 20-24% -> 6% of the samples (flat), ~1% on lean memory.

### 3. Write traps on code pages (flat memory)

The flat build write-protects guarded code pages; each store to one is an access violation, a protection change to
let it through and another to guard the page again at the next Sync. P+ keeps a variable inside sora_melee's code
(0x807824D4, the word after its hook at 0x807824CC, which the patched code jumps over): ~70 traps a frame on that page
alone, plus 2 a frame on each of 13 edge pages. NtProtectVirtualMemory + exception dispatch were 5-7% of the samples.
Lean memory (kalomaze PR #5) checks stores inline and has none of this.

### 4. Lean memory on Windows (PR #5)

PR #5 merged onto 66a8cd0 builds and plays on Windows (llvm-mingw); replay 004 passes its checksums. Under the rollback
stress (distance 4, replay 004) it **logged garbage display-list reads in 3 of 4 runs** (with or without a510744; the
flat build had none in 2): the g3d native draw worker (`G3D_NATIVE_ASYNC=2`,
`GX__CallRegisterDisplayList_G3D` -> `WalkDisplayList` -> `Memory::GetPointer`) asked for lengths like 0x80BD4160 from
`ctx=(null)`. With code variants it did not happen in 2 runs (timing may hide it). Looks like the worker reading guest
memory a rollback load is rewriting: `SnapshotRestore` rewrote guest memory without the GX sync that g3d_native.cpp
counts on before anything the worker reads is reused. Fixed in c221f8f (a GX sync first, ~0.045 ms a load): 0 errors in
the 10 sync-test runs after it, against about half of the 14 before (lean; the flat build's race is the same, it just
didn't show in its runs).

### 5. What remains after Sync, lean memory and variants

Lean + a510744 + variants, replay 004: translated game code 47%, other runtime 14% (input sampling, matrix natives,
texture lookups), ntdll/ucrtbase 11%, GX HLE / aurora 10%, interpreter 9%, indirect dispatch 6%. No single hotspot:
the largest translated function is about 1% self. Further gains there are code-generation-wide (the sheet's register
spills, condition-register round trips, indirect calls).

Other things the caller attribution showed on the game thread, all small on a quiet PC and larger under load:

- waits for the GX thread at its syncs (draw-done, copy filter, frame end) and small inline decodes in
  `aurora::gx::worker`: 1-7% of samples depending on load (the sheet's GPU-thread item, PR #6);
- the audio tick joining the AX mix worker: 1-5%;
- SDL's event pump (`aurora::window::poll_events`, NtDeviceIoControlFile): 0.3% in one run and 6% in another, depending
  on what input devices were attached at the time (other sessions use virtual gamepads);
- `VirtualRemote::Sample` (the host's stand-in Wii Remote for port 1, polled every frame): ~4% of samples total;
- log writes (fprintf on the game thread): ~1%.

## Timings

Replay 004, the machine under 15% CPU before each run, builds interleaved, three rounds each (medians; the
round-to-round spread was under 2% on a quiet machine). "Variants" = the P+ code-variant spec above.

Unthrottled playback, the game thread's CPU per frame:

| build | ms/frame | vs base |
|---|---|---|
| base (66a8cd0, flat memory: what releases are) | 2.40 | |
| + a510744 (Sync) | 1.92 | -20% |
| + a510744 + variants | 1.51 | -37% |
| lean memory (PR #5) | 1.96 | -18% |
| lean + a510744 | 1.45 | -40% |
| lean + a510744 + variants | 1.18 | -51% |

Rollback stress (PR #3's probe, distance 4: every frame rolls back 4 and runs them again), medians in ms:

| build | update (4 frames) | shown frame | re-run frame | save | load |
|---|---|---|---|---|---|
| base | 11.21 | 3.08 | 2.59 | 0.030 | 0.062 |
| + a510744 | 9.33 | 2.61 | 2.11 | 0.030 | 0.060 |
| + a510744 + variants | 7.81 | 2.23 | 1.74 | 0.029 | 0.059 |
| lean | 7.84 | 2.10 | 1.78 | 0.041 | 0.096 |
| lean + a510744 | 5.74 | 1.59 | 1.25 | 0.039 | 0.094 |
| lean + a510744 + variants | 5.00 | 1.42 | 1.07 | 0.042 | 0.100 |

Saves and loads are not where the time goes (under 0.1 ms; lean's are ~0.04 ms slower). A re-run frame is game code.

## Checks

- The 50-replay corpus (Dolphin recordings, every frame's checksum against the recording), playback at normal speed:
  flat + a510744 + variants: 49 passed (545,514 frames), 038 stopped by the runner's 900 s limit at frame 11,165 of
  11,176 with no mismatch while the PC was loaded (80 ms a frame). The flat build of `brawlonline/perf` (a510744,
  bb761c0, c221f8f, no variants): 50/50, 556,690 frames. That branch merged into windows-build (with PR #6, NaN and CR
  fields) builds and passes replay 004 with main's core.
- Replay 004 passes on every build in the tables (flat and lean, with and without variants); replays 010, 020 and 030
  on flat + variants.
- Unthrottled runs with several instances lost the Vulkan device now and then (VK_ERROR_DEVICE_LOST, nvlddmkm event 153
  and Display 4101 in the system log at the same minutes): a driver reset under the GPU load of several games
  presenting hundreds of frames a second, not these changes. Corpus runs go at normal speed for that reason.

## What to do next, by size

1. **Code variants for Project+ in releases**: done after this write-up (Sarah: "you can ship P+ stuff ... use the
   same game-key as before"). `tools/pplus_variants.py` makes the spec from a replay, `game_code.py pack-variants`
   encrypts it with the game key, release CI builds with it when `pplus-variants.tar.gz.gpg` is in the game-data
   repository (`docs/releases.md`), and staging builds with it when it is beside the CI's game code. A spec made from
   the launcher's 11-26 card: 1,170 of 1,209 functions translated, the executable 9.9 MB larger (244.0 MB), 0.41-0.43
   ms less game-thread time a frame in three same-time rounds against the same build without it.

   What this is and isn't: the build translates, ahead of time and with the same translator as the rest of the game,
   copies of the disc's functions *as Project+'s codes leave them* (hooks and all); each runs only while guest memory
   holds exactly the words it was made from, else the interpreter does. It is native code chosen per function at run
   time, not a JIT. It covers code a known mod writes into the disc's functions; it doesn't cover a mod nobody made a
   spec for, a mod version after the spec, or code that only exists at run time (a mod's own modules linked in place,
   our plugin), which still go to the interpreter. vBrawl needs none: with no codes, everything it runs is already
   translated. REX, Cobalt and others would each need their own spec (the same script, one replay each); only the
   variants of the mod that is running match, so several can be built in at once, each costing executable size.

   A faster interpreter or a JIT (Bella's work) is the other half, not a rival: it would take exactly the calls that
   fall back today (an unknown mod, an old spec, mod modules, the plugin). With variants in, a JIT also has far less
   P+ code to compile at a match's start.
2. **Lean memory (PR #5)** (-18% alone, -25 to -30% with a510744). Builds and plays on Windows; the display-list race it
   exposed is fixed by c221f8f. No write traps, inline dirty marks.
3. a510744 (Sync) and c221f8f are in `brawlonline/perf`.
4. After those, the game thread is ~half translated game code with no hotspot: the sheet's code-generation items
   (register spills, CR round trips, indirect calls) are where the rest is, plus the GX-thread syncs (PR #6).
5. Small, worth a look: `VirtualRemote::Sample` (~4%: port 1's stand-in Wii Remote reads the keyboard, every gamepad and
   the mouse each frame, also in re-run frames) and SDL's event pump on the game thread (its cost depends on the input
   devices attached).
