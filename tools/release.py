#!/usr/bin/env python3
"""Builds an EmissaryRecomp release with Brawl Online's online core: both Windows engines, the Linux program (in a WSL
distro from Windows, or directly on Linux), the macOS program (on a Mac), the downloads and manifest.json; with
--publish, the GitHub release. The layout and the manifest are described in docs/releases.md.

    python tools/release.py --game <extracted disc DATA> [--prerelease rc.1] [--publish]

The version is VERSION (major.minor.patch); the release is the tag v<VERSION>, or v<VERSION>-<prerelease> for a
prerelease (GitHub never offers a prerelease as /releases/latest, so no launcher installs it by itself). CI passes the
tag it was started for with --tag. A release is built from a commit (the manifest names it): the working tree must be
clean unless --allow-dirty (a local test, never published).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
PRODUCT = 'EmissaryRecomp'
TOP = PRODUCT  # the archives' one top folder (the launcher looks one folder down for the program)
REPO = 'kalomaze/EmissaryRecomp'
ENGINE = 'emissary'  # the engine's name as Brawl Online's servers learn it (brawlonline.net, brawl-online-core.md §10)
PLATFORMS = {
    'windows': {'asset': '{p}-{tag}-Windows.zip', 'program': 'WiiCompiled.exe'},
    'linux': {'asset': '{p}-{tag}-Linux.tar.gz', 'program': 'WiiCompiled'},
    'macos': {'asset': '{p}-{tag}-macOS.tar.gz', 'program': 'WiiCompiled'},  # Apple silicon
}
TAG = re.compile(r'^v(\d+\.\d+\.\d+)(?:-([0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*))?$')


def log(message):
    print(f'== {message}', flush=True)


def run(*args, cwd=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def git_out(*args) -> str:
    return subprocess.run(['git', '-C', str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def wsl_path(path: Path) -> str:
    """A Windows path as WSL sees it (/mnt/<drive>/...)."""
    path = path.resolve()
    drive = path.drive.rstrip(':').lower()
    if not drive or len(drive) != 1:
        raise RuntimeError(f'{path}: WSL reaches only drive-letter paths')
    return f'/mnt/{drive}/' + '/'.join(path.parts[1:])


def release_tag(args) -> tuple[str, bool]:
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise RuntimeError(f'VERSION is {version!r}, not major.minor.patch')
    tag = args.tag or f'v{version}' + (f'-{args.prerelease}' if args.prerelease else '')
    match = TAG.match(tag)
    if not match:
        raise RuntimeError(f'{tag}: a release tag is v<major.minor.patch>[-<prerelease>]')
    if match.group(1) != version:
        raise RuntimeError(f'{tag} is not VERSION ({version}): bump VERSION in the tagged commit')
    return tag, match.group(2) is not None


def build_windows(args, tag):
    """Both engines: engine_usa.dll (x86-64-v3, AVX2/FMA) in build/release, engine_usa_v2.dll (x86-64-v2) in
    build/release-v2; the loader (WiiCompiled.exe) picks one by the CPU."""
    script = TOOLS / 'build-brawl.ps1'
    common = ['-NoPlay', '-BrawlOnline', 'lock', '-ReleaseVersion', tag, '-Game', args.game, '-Jobs', args.jobs]
    for extra in ([], ['-OldCpu']):
        log(f'Windows engine {"v2 (no AVX2)" if extra else "v3"}')
        run('powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script, *common, *extra)


def build_linux(args, tag, commit, out: Path):
    """The Linux program, built from the commit in Ubuntu 24.04 (tools/release-linux.sh); its files go to out."""
    out.mkdir(parents=True, exist_ok=True)
    script_args = [str(args.jobs), tag, commit]
    if os.name == 'nt':
        game = args.linux_game or wsl_path(args.game)
        log(f'Linux program, in WSL ({args.wsl_distro})')
        run('wsl', '-d', args.wsl_distro, '--', 'bash', wsl_path(TOOLS / 'release-linux.sh'), wsl_path(ROOT), game,
            wsl_path(out), *script_args)
    else:
        log('Linux program')
        run('bash', TOOLS / 'release-linux.sh', ROOT, args.linux_game or args.game.resolve(), out, *script_args)


def build_macos(args, tag, commit, out: Path):
    """The macOS program, built from the commit on this Mac (tools/release-macos.sh); its files go to out. A release
    with Windows too is packaged on the Windows machine: copy out (dist/<tag>/macos-native) there and --skip-build."""
    out.mkdir(parents=True, exist_ok=True)
    log('macOS program')
    run('bash', TOOLS / 'release-macos.sh', ROOT, args.game.resolve(), out, str(args.jobs), tag, commit)


def copy_tree(src: Path, dst: Path):
    if not src.is_dir():
        raise RuntimeError(f'missing {src}')
    shutil.copytree(src, dst, dirs_exist_ok=True)


def stage_common(top: Path, info: dict, newline: str):
    """What every platform's download has besides the program: licenses, credits, the title card's art, release.json."""
    (top / 'licenses').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / 'wiicompiled/LICENSE', top / 'LICENSE.txt')
    shutil.copyfile(TOOLS / 'CREDITS.txt', top / 'CREDITS.txt')
    shutil.copyfile(ROOT / 'wiicompiled/THIRD-PARTY-NOTICES.md', top / 'licenses/THIRD-PARTY-NOTICES.md')
    shutil.copyfile(ROOT / 'wiicompiled/runtime/third_party/fatfs/LICENSE.txt', top / 'licenses/FatFs-LICENSE.txt')
    dependencies = ROOT / 'wiicompiled/Dependencies'  # build-brawl.ps1's pinned trees (Prepare-Dependencies.ps1)
    for folder in sorted(p for p in dependencies.iterdir() if p.is_dir()) if dependencies.is_dir() else []:
        for f in sorted(folder.iterdir()):
            if f.is_file() and re.match(r'^(LICEN[CS]E|COPYING|NOTICE)', f.name):
                shutil.copyfile(f, top / 'licenses' / f'{folder.name}-{f.stem}.txt')
    (top / 'startup').mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'launcher/art/smash_ball.png', top / 'startup/smash_ball.png')  # the title card's Smash Ball
    text = (TOOLS / 'release-README.txt').read_text(encoding='utf-8')
    for key, value in (('{VERSION}', info['version']), ('{CORE}', info['brawlOnline'])):
        text = text.replace(key, value)
    (top / 'README.txt').write_text(text, encoding='utf-8', newline=newline)
    (top / 'release.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8', newline='\n')


def guard(top: Path):
    # No disc data, settings, saves or SD card in a download: the player's own are kept beside the program.
    for p in top.rglob('*'):
        if p.is_dir() and p.name in ('Disc', 'DATA', 'sys', 'files', 'UserData'):
            raise RuntimeError(f'{p}: no disc, UserData or SD card folders in a release')
        if p.name in ('portable.txt', 'Config.toml', 'sd.raw') or p.suffix in ('.dol', '.rel', '.iso', '.wbfs'):
            raise RuntimeError(f'{p}: not part of a release')


def stage_windows(stage: Path, info: dict) -> Path:
    v3, v2 = ROOT / 'build/release/native', ROOT / 'build/release-v2/native'
    top = stage / TOP
    if stage.exists():
        shutil.rmtree(stage)
    top.mkdir(parents=True)
    for need in (v3 / 'WiiCompiled.exe', v3 / 'engine_usa.dll', v2 / 'engine_usa_v2.dll', v3 / 'Sys/Rollback'):
        if not need.exists():
            raise RuntimeError(f'missing {need}: build both engines with the core (build-brawl.ps1 -BrawlOnline lock)')
    shutil.copyfile(v3 / 'WiiCompiled.exe', top / 'WiiCompiled.exe')  # the loader
    for dll in sorted(v3.glob('*.dll')):  # the engine and the libraries beside it (SDL3, libc++, libpng...)
        if dll.name.lower() != 'engine_usa_v2.dll':
            shutil.copyfile(dll, top / dll.name)
    shutil.copyfile(v2 / 'engine_usa_v2.dll', top / 'engine_usa_v2.dll')
    shutil.copyfile(v3 / 'dsp_coef.bin', top / 'dsp_coef.bin')
    copy_tree(v3 / 'wii_bootstrap', top / 'wii_bootstrap')
    copy_tree(v3 / 'Sys', top / 'Sys')  # Brawl Online's region sets (Sys/Rollback), read beside the program
    stage_common(top, info, '\r\n')
    guard(top)
    return top


def stage_linux(stage: Path, native: Path, info: dict, system: str = 'Linux') -> Path:
    """The Linux download, or the macOS one (the same layout: the program and its files, LF text)."""
    top = stage / TOP
    if stage.exists():
        shutil.rmtree(stage)
    top.mkdir(parents=True)
    for need in ('WiiCompiled', 'dsp_coef.bin', 'cacert.pem', 'wii_bootstrap', 'Sys/Rollback'):
        if not (native / need).exists():
            raise RuntimeError(f'missing {native / need}: the {system} build did not finish')
    for f in sorted(native.iterdir()):
        if f.is_file():  # WiiCompiled, any library it bundles, dsp_coef.bin, cacert.pem
            shutil.copyfile(f, top / f.name)
    copy_tree(native / 'wii_bootstrap', top / 'wii_bootstrap')
    copy_tree(native / 'Sys', top / 'Sys')
    stage_common(top, info, '\n')
    guard(top)
    return top


def write_zip(top: Path, archive: Path):
    # Entries with '/' separators and the one top folder; empty folders kept.
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(top.rglob('*')):
            name = PurePosixPath(TOP, *p.relative_to(top).parts).as_posix()
            if p.is_file():
                z.write(p, name)
            elif not any(p.iterdir()):
                z.writestr(name + '/', '')


def write_tar(top: Path, archive: Path):
    # Modes set here, not taken from the disk (files built in WSL and copied to Windows lose theirs): the program and
    # its shared libraries 0755, the rest 0644, owned by no one in particular.
    def entry(info: tarfile.TarInfo) -> tarfile.TarInfo:
        name = PurePosixPath(info.name).name
        executable = info.isdir() or name == 'WiiCompiled' or '.so' in name or name.endswith('.dylib')
        info.mode = 0o755 if executable else 0o644
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        return info

    with tarfile.open(archive, 'w:gz', compresslevel=9, format=tarfile.PAX_FORMAT) as t:
        t.add(top, arcname=TOP, filter=entry)


def publish(args, tag, prerelease, commit, files):
    repo = args.repo
    remote_tag = subprocess.run(['git', '-C', str(ROOT), 'ls-remote', '--tags', f'https://github.com/{repo}.git',
                                 f'refs/tags/{tag}'], check=True, capture_output=True, text=True).stdout.split()
    if remote_tag and remote_tag[0] != commit:
        # An annotated tag lists its object, not the commit: compare the peeled one.
        peeled = subprocess.run(['git', '-C', str(ROOT), 'ls-remote', '--tags', f'https://github.com/{repo}.git',
                                 f'refs/tags/{tag}^{{}}'], check=True, capture_output=True, text=True).stdout.split()
        if not peeled or peeled[0] != commit:
            raise RuntimeError(f'{repo} already has {tag} at another commit')
    notes = args.out / 'notes.md'
    notes.write_text(
        f'EmissaryRecomp {tag} with Brawl Online\'s online core {files["info"]["brawlOnline"]} '
        f'({files["info"]["core"]["commit"][:10]}), built from {commit[:10]}.\n\n'
        'Brawl Online\'s launcher installs and updates it (Settings > Add-ons). The downloads hold the game program '
        'only; it plays the files of your own disc.\n' +
        ('\nA prerelease: launchers do not install it unless pointed at it.\n' if prerelease else ''),
        encoding='utf-8')
    command = ['gh', 'release', 'create', tag, *map(str, files['assets']), '--repo', repo, '--target', commit,
               '--title', f'{PRODUCT} {tag}', '--notes-file', str(notes)]
    # Never /releases/latest by itself (what every launcher installs): a prerelease never is, a release only with
    # --latest (or later: gh release edit <tag> --latest).
    command += ['--prerelease', '--latest=false'] if prerelease else ['--latest' if args.latest else '--latest=false']
    run(*command)
    if not prerelease and not args.latest:
        log(f'{tag} is published but not the latest release: launchers install it once it is '
            f'(gh release edit {tag} --repo {repo} --latest)')


def write_sums(out: Path, files: list[Path]):
    (out / 'SHA256SUMS').write_text(''.join(f'{sha256(f)}  {f.name}\n' for f in files), encoding='utf-8', newline='\n')


def publish_existing(args, tag, prerelease, commit):
    """Publishes what an earlier run left in the output folder, byte for byte (packaging again would change the
    archives' timestamps, so they would no longer be the files that were tried), after checking it is that release."""
    out = args.out
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('version') != tag:
        raise RuntimeError(f'{out / "manifest.json"} is {manifest.get("version")}, not {tag}')
    built = manifest.get('commit', '')
    # The release names the commit its files were built from: this checkout or one before it (the tool may have
    # changed since), never another branch's.
    if built != commit and subprocess.run(['git', '-C', str(ROOT), 'merge-base', '--is-ancestor', built, commit]).returncode:
        raise RuntimeError(f'those files were built from {built}, which is not in the history of this checkout')
    assets = []
    for platform, entry in manifest['platforms'].items():
        archive = out / entry['asset']
        if sha256(archive) != entry['sha256'] or archive.stat().st_size != entry['size']:
            raise RuntimeError(f'{archive} is not the file manifest.json describes')
        assets.append(archive)
    sums = out / 'SHA256SUMS'
    for line in sums.read_text(encoding='utf-8').splitlines():
        digest, name = line.split(maxsplit=1)
        if sha256(out / name) != digest:
            raise RuntimeError(f'{out / name} does not match SHA256SUMS')
    log(f'publishing {tag} ({", ".join(manifest["platforms"])}) from {out}')
    publish(args, tag, prerelease, built, {'assets': [*assets, out / 'manifest.json', sums], 'info': manifest})


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', type=Path, help='the extracted disc (DATA: sys/main.dol, files/)')
    parser.add_argument('--linux-game', help='the same, as the Linux build sees it (default: --game through /mnt)')
    parser.add_argument('--tag', help='the release tag (default: v<VERSION>[-<prerelease>])')
    parser.add_argument('--prerelease', help='a prerelease of VERSION: rc.1 makes v<VERSION>-rc.1')
    parser.add_argument('--platforms', default='windows,linux',
                        help='comma-separated: windows, linux, macos (macos builds on a Mac)')
    parser.add_argument('--jobs', type=int, default=6, help='compile jobs per build (one build at a time)')
    parser.add_argument('--wsl-distro', default='EmissaryRecomp-Build', help='the WSL distro of the Linux build')
    parser.add_argument('--skip-build', action='store_true', help='package the builds already there')
    parser.add_argument('--allow-dirty', action='store_true', help='build a tree with uncommitted changes (no publish)')
    parser.add_argument('--out', type=Path, help='output folder (default: dist/<tag>)')
    parser.add_argument('--publish', action='store_true', help='create the GitHub release (gh, GH_TOKEN or gh auth)')
    parser.add_argument('--latest', action='store_true',
                        help="make a published release (not a prerelease) /releases/latest, what launchers install")
    parser.add_argument('--publish-existing', action='store_true',
                        help='publish the files already in the output folder (built and tested before), unchanged')
    parser.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', REPO))
    args = parser.parse_args()
    if args.prerelease and args.tag:
        parser.error('--tag or --prerelease, not both')
    platforms = [p.strip() for p in args.platforms.split(',') if p.strip()]
    if not platforms or any(p not in PLATFORMS for p in platforms):
        parser.error(f'--platforms: {", ".join(PLATFORMS)}')
    if 'windows' in platforms and os.name != 'nt' and not args.skip_build:
        parser.error('the Windows engines build on Windows (--skip-build packages builds made there)')
    if 'macos' in platforms and sys.platform != 'darwin' and not args.skip_build:
        parser.error('the macOS program builds on a Mac (--skip-build packages its macos-native folder)')
    tag, prerelease = release_tag(args)
    dirty = git_out('status', '--porcelain', '--untracked-files=no')
    if dirty and not args.allow_dirty:
        raise RuntimeError(f'uncommitted changes (a release names its commit):\n{dirty}')
    if dirty and args.publish:
        raise RuntimeError('a tree with uncommitted changes is never published')
    commit = git_out('rev-parse', 'HEAD')
    lock = json.loads((ROOT / 'brawl_online.lock').read_text())
    for key in ('repo', 'commit', 'appVersion', 'protocol'):
        if key not in lock:
            raise RuntimeError(f'brawl_online.lock has no {key}')
    out = (args.out or ROOT / 'dist' / tag).resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.out = out
    if args.publish_existing:
        if dirty:
            raise RuntimeError('a tree with uncommitted changes is never published')
        publish_existing(args, tag, prerelease, commit)
        return
    if not args.skip_build and (not args.game or not (args.game / 'sys/main.dol').is_file()):
        raise RuntimeError(f'--game {args.game}: no sys/main.dol (the extracted disc: DATA)')
    log(f'{PRODUCT} {tag} from {commit}{" (with uncommitted changes)" if dirty else ""}; Brawl Online '
        f'{lock["appVersion"]} at {lock["commit"]}; {", ".join(platforms)}')

    if not args.skip_build:
        if 'linux' in platforms:  # first: it checks out the commit itself, so it fails early on an unpushed tree
            build_linux(args, tag, commit, out / 'linux-native')
        if 'macos' in platforms:
            build_macos(args, tag, commit, out / 'macos-native')
        if 'windows' in platforms:
            build_windows(args, tag)

    # What the release is, in the downloads (release.json) and beside them (manifest.json, with the assets).
    info = {
        'version': tag,
        'commit': commit,
        'brawlOnline': lock['appVersion'],
        'protocol': lock['protocol'],
        'core': {'repo': lock['repo'], 'commit': lock['commit'], 'appVersion': lock['appVersion']},
        'engine': {'name': ENGINE, 'version': tag},
    }
    assets, manifest_platforms = [], {}
    for platform in platforms:
        name = PLATFORMS[platform]['asset'].format(p=PRODUCT, tag=tag)
        archive = out / name
        log(f'{name}')
        if platform == 'windows':
            top = stage_windows(out / 'stage-windows', info)
            write_zip(top, archive)
        elif platform == 'macos':
            top = stage_linux(out / 'stage-macos', out / 'macos-native', info, 'macOS')
            write_tar(top, archive)
        else:
            top = stage_linux(out / 'stage-linux', out / 'linux-native', info)
            write_tar(top, archive)
        assets.append(archive)
        manifest_platforms[platform] = {'asset': name, 'size': archive.stat().st_size, 'sha256': sha256(archive),
                                        'program': f'{TOP}/{PLATFORMS[platform]["program"]}'}
    manifest = {**info, 'prerelease': prerelease, 'platforms': manifest_platforms}
    # LF on every system (Python on Windows writes CRLF by default, and `sha256sum -c` reads the CR as part of the name).
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    assets.append(out / 'manifest.json')
    write_sums(out, assets)
    assets.append(out / 'SHA256SUMS')
    for a in assets:
        log(f'{a} ({a.stat().st_size / 1e6:.1f} MB)')
    if args.publish:
        publish(args, tag, prerelease, commit, {'assets': assets, 'info': info})


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'Release failed: {error}')
