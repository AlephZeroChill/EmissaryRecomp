#!/usr/bin/env python3
"""Project+'s code-variant spec (docs/performance.md, "The interpreter is Project+'s patches"): plays one Brawl Online
replay through a build of this repository, dumps P+'s patched code at the match's first frame, and makes the spec with
projects/brawl/tools/gen_variant_spec.py. A build with the spec (-DCODE_VARIANTS=<out>/spec.txt, or
EMISSARY_CODE_VARIANTS) runs P+'s patched functions translated instead of in the interpreter.

    python tools/pplus_variants.py --build build/native --replay <game.brep> --card <Brawl Online's sd.raw> \
        --out <spec dir> [--user-config <Brawl Online user Config folder>]

--build: a build without variants (its source/wiicompiled/generated holds the function table the spec is made
against), with UserData's NAND and Codes as Brawl Online's launcher sets them up (the card's NETPLAY.GCT as
Codes/RSBE01.gct). --card: the online SD card the replay was played with (the P+ version and plugin the variants are
for; read only). Any replay that reaches a match will do: the patches are the code lists' and the plugin's, the same
for every character and stage. A later P+ card or plugin leaves the variants whose words changed to the interpreter
(checked at run time) until the spec is made again.

Then: python tools/ci/game_code.py pack-variants --spec <out>/spec.txt --out pplus-variants.tar.gz.gpg (the release
CI's game-data repository; the dumps are P+'s code lists and the plugin as they sit in memory, never public).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# P+'s code handler and its two code lists, main.dol's code, the resident modules, Syriinge's heap (sy_core's hooks).
DUMP_RANGES = ('80001800-80003000,80550000-80566000,80566000-80580000,80004000-80406500,80673F00-80B85000,'
               '81050000-81070000')


def link_or_copy(src: Path, dst: Path):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def make_instance(build: Path, card: Path, user_config: Path | None, d: Path) -> Path:
    d.mkdir(parents=True)
    for p in build.iterdir():
        if p.is_file() and p.suffix.lower() in ('.dll', '.exe', '.pem', '.bin', '.db', '.txt'):
            link_or_copy(p, d / p.name)
    for sub in ('Sys', 'wii_bootstrap'):
        shutil.copytree(build / sub, d / sub)
    ud = d / 'UserData'
    ud.mkdir()
    shutil.copyfile(build / 'UserData/Config.toml', ud / 'Config.toml')
    for sub in ('NAND', 'Codes'):
        if not (build / 'UserData' / sub).is_dir():
            raise SystemExit(f'{build}/UserData/{sub} is missing (the launcher\'s NAND and the card\'s codes)')
        shutil.copytree(build / 'UserData' / sub, ud / sub)
    link_or_copy(card, ud / 'sd.raw')
    (d / 'user/Config').mkdir(parents=True)
    if user_config:
        for f in ('Dolphin.ini', 'Logger.ini', 'GFX.ini'):
            if (user_config / f).is_file():
                shutil.copyfile(user_config / f, d / 'user/Config' / f)
    return d


def play(d: Path, replay: Path, dump_prefix: Path, timeout: float) -> Path:
    user = d / 'user'
    comm = user / 'comm.json'
    status = user / 'comm.json.status.json'
    comm.write_text(json.dumps({'replay': str(replay).replace('\\', '/'), 'commandId': replay.name}))
    env = dict(os.environ, BRAWL_ONLINE_CORE=str(user), BRAWL_ONLINE_REPLAY_COMM=str(comm),
               BRAWL_SD_VOLATILE_WRITES='1', SDL_JOYSTICK_HIDAPI_GAMECUBE='0', SDL_JOYSTICK_HIDAPI='0',
               # The scene hook that dumps runs only with a pad script; this one never presses anything.
               BRAWL_PAD_SCRIPT='nosuchscene+99999:A',
               BRAWL_DUMP_ON_SCENE=f'scMelee:{DUMP_RANGES}:{dump_prefix.as_posix()}')
    log = d / 'stderr.txt'
    started = time.monotonic()
    with open(log, 'wb') as err:
        p = subprocess.Popen([str(d / 'WiiCompiled.exe' if os.name == 'nt' else d / 'WiiCompiled')], cwd=d, env=env,
                             stdout=subprocess.DEVNULL, stderr=err)
        try:
            while time.monotonic() - started < timeout:
                time.sleep(2)
                if p.poll() is not None:
                    raise SystemExit(f'the game exited ({p.returncode}) before the replay ended; see {log}')
                try:
                    st = json.loads(status.read_text())
                except (OSError, ValueError):
                    continue
                if st.get('commandId') != replay.name:
                    continue
                if st.get('error') or (st.get('desync_frame') or -1) >= 0:
                    raise SystemExit(f'the replay did not play back cleanly: {st}')
                if st.get('at_end'):
                    break
            else:
                raise SystemExit(f'the replay did not end in {timeout:.0f} s; see {log}')
        finally:
            p.kill()
            p.wait()
    if not list(dump_prefix.parent.glob(dump_prefix.name + '_*.bin')):
        raise SystemExit(f'no dumps: the replay never reached a match (scMelee); see {log}')
    return log


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', type=Path, required=True, help='the native build folder (WiiCompiled, Sys, UserData)')
    ap.add_argument('--replay', type=Path, required=True)
    ap.add_argument('--card', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--user-config', type=Path, help="Brawl Online's user Config folder (Dolphin.ini etc.)")
    ap.add_argument('--timeout', type=float, default=1200)
    args = ap.parse_args()
    build = args.build.resolve()
    wiicompiled = build.parent / 'source/wiicompiled'
    if not list((wiicompiled / 'generated/build_shards/base_dispatch').glob('guest_code_guard_*.cpp')):
        raise SystemExit(f'{wiicompiled}/generated has no guard table: --build must be a translated build folder')
    if args.out.exists() and any(args.out.iterdir()):
        raise SystemExit(f'{args.out} is not empty')
    work = Path(tempfile.mkdtemp(prefix='pplus-variants-'))
    try:
        d = make_instance(build, args.card.resolve(), args.user_config, work / 'instance')
        (work / 'dump').mkdir()
        log = play(d, args.replay.resolve(), work / 'dump/pp', args.timeout)
        subprocess.run([sys.executable, str(ROOT / 'wiicompiled/projects/brawl/tools/gen_variant_spec.py'),
                        str(wiicompiled), str(work / 'dump/pp'), str(args.out), '--patched-log', str(log)], check=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f'{args.out / "spec.txt"}: build with -DCODE_VARIANTS={args.out / "spec.txt"} (or EMISSARY_CODE_VARIANTS)')


if __name__ == '__main__':
    main()
