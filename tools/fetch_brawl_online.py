"""Fetches the part of Brawl Online (brawlonline.net) this build links: its online core and what the core's standalone
build uses (core/standalone/CMakeLists.txt: Dolphin's Common, Brawlback's headers, four of Dolphin's rollback files, the
region sets in Data/Sys/Rollback and ten of Dolphin's Externals, seven of them git submodules), at the commit
brawl_online.lock pins. A blob-less sparse clone of the repository and each submodule's one pinned commit (depth 1, no
tags), so a few tens of MB and about a minute instead of the whole repository.

    python tools/fetch_brawl_online.py <destination>            # the lock's repository and commit
    python tools/fetch_brawl_online.py <destination> --repo <url or path> --commit <sha> [--app-version 0.2.3]

The core reports Online::APP_VERSION (core/include/BrawlOnline/Online/User.h) to Brawl Online's servers as appVersion,
which they compare with Brawl Online's own releases; the repository has a placeholder there that Brawl Online's CI
replaces when it builds a release (.github/scripts/stamp-version.sh), so this does the same with the lock's appVersion:
the Brawl Online release the pinned core belongs to.

Prints the checkout's path (BRAWL_ONLINE_ROOT for the native configure). Reuses <destination> when it already holds
the commit and appVersion.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'brawl_online.lock'

PATHS = [
    'core/',
    'dolphin/Source/Core/Common/',
    'dolphin/Source/Core/Core/Brawlback/include/',
    'dolphin/Source/Core/InputCommon/GCPadStatus.h',
    'dolphin/Source/Core/Core/Rollback/FrameProfile.cpp',
    'dolphin/Source/Core/Core/Rollback/FrameProfile.h',
    'dolphin/Source/Core/Core/Rollback/UdpSocket6.cpp',
    'dolphin/Source/Core/Core/Rollback/UdpSocket6.h',
    'dolphin/Source/Core/Core/Rollback/ReplayPlayback.cpp',
    'dolphin/Source/Core/Core/Rollback/ReplayPlayback.h',
    'dolphin/Source/Core/Core/Rollback/GameplayOnlineBackend.cpp',
    'dolphin/Source/Core/Core/Rollback/GameplayOnlineBackend.h',
    'dolphin/Data/Sys/Rollback/',
    'dolphin/Externals/SFML/',
    'dolphin/Externals/curl/',
    'dolphin/Externals/enet/',
    'dolphin/Externals/fmt/',
    'dolphin/Externals/mbedtls/',
    'dolphin/Externals/miniupnpc/',
    'dolphin/Externals/monocypher/',
    'dolphin/Externals/picojson/',
    'dolphin/Externals/xxhash/',
    'dolphin/Externals/zlib-ng/',
    '.gitmodules',
]
SUBMODULES = [
    'dolphin/Externals/SFML/SFML',
    'dolphin/Externals/curl/curl',
    'dolphin/Externals/enet/enet',
    'dolphin/Externals/fmt/fmt',
    'dolphin/Externals/miniupnpc/miniupnp',
    'dolphin/Externals/xxhash/xxHash',
    'dolphin/Externals/zlib-ng/zlib-ng',
]
USER_H = 'core/include/BrawlOnline/Online/User.h'
VERSION = re.compile(r"^\d+\.\d+\.\d+$")  # what Brawl Online releases are (its CI: 0.2.<run>)


def git(*args, cwd=None):
    subprocess.run(['git', '-c', 'advice.detachedHead=false', *args], cwd=cwd, check=True)


def git_out(*args, cwd=None) -> str:
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def submodule_url(dest: Path, path: str) -> str:
    # .gitmodules: submodule.<name>.path = <path>, submodule.<name>.url = <url>
    paths = git_out('config', '-f', '.gitmodules', '--get-regexp', r'^submodule\..*\.path$', cwd=dest)
    for line in paths.splitlines():
        key, _, value = line.partition(' ')
        if value == path:
            return git_out('config', '-f', '.gitmodules', '--get', key[:-len('.path')] + '.url', cwd=dest)
    raise RuntimeError(f'{path} is not in .gitmodules')


def fetch_submodule(dest: Path, path: str):
    # The commit the superproject pins (a gitlink in its tree), fetched alone: `git submodule update --depth 1` also
    # fetches every tag (curl has thousands), which took most of the 9 minutes this used to take.
    entry = git_out('ls-tree', 'HEAD', '--', path, cwd=dest).split()
    if len(entry) < 3 or entry[1] != 'commit':
        raise RuntimeError(f'{path} is not a submodule at this commit')
    sha, url = entry[2], submodule_url(dest, path)
    sub = dest / path
    sub.mkdir(parents=True, exist_ok=True)
    git('init', '-q', cwd=sub)
    git('remote', 'add', 'origin', url, cwd=sub)
    git('-c', 'protocol.version=2', 'fetch', '-q', '--depth', '1', '--no-tags', 'origin', sha, cwd=sub)
    git('checkout', '-q', 'FETCH_HEAD', cwd=sub)


def stamp_app_version(dest: Path, version: str):
    user_h = dest / USER_H
    text = user_h.read_text(encoding='utf-8')
    text, n = re.subn(r'(?m)^(constexpr char APP_VERSION\[\] = )"[^"]*";', rf'\1"{version}";', text)
    if n != 1:
        raise RuntimeError(f'could not stamp APP_VERSION in {user_h}')
    user_h.write_text(text, encoding='utf-8', newline='')


def remove_tree(path: Path):
    # git's object files are read-only, which Windows refuses to delete: make them writable first.
    def writable(function, name, _error):
        os.chmod(name, stat.S_IWRITE)
        function(name)
    shutil.rmtree(path, onexc=writable)


def fetch(dest: Path, repo: str, commit: str, app_version: str | None) -> Path:
    stamp = dest / '.brawl-online-commit'
    # The list of paths is part of what was fetched: a checkout made with an older list is fetched again.
    paths = hashlib.sha256(' '.join(PATHS + SUBMODULES).encode()).hexdigest()[:12]
    want = f'{commit} {app_version or "-"} {paths}'
    if stamp.exists() and stamp.read_text().strip() == want:
        return dest
    if dest.exists():
        remove_tree(dest)
    dest.mkdir(parents=True)
    git('init', '-q', cwd=dest)
    git('remote', 'add', 'origin', repo, cwd=dest)
    git('config', 'core.sparseCheckout', 'true', cwd=dest)
    git('sparse-checkout', 'set', '--no-cone', *PATHS, cwd=dest)
    git('-c', 'protocol.version=2', 'fetch', '-q', '--depth', '1', '--no-tags', '--filter=blob:none', 'origin', commit,
        cwd=dest)
    git('checkout', '-q', 'FETCH_HEAD', cwd=dest)
    with concurrent.futures.ThreadPoolExecutor(len(SUBMODULES)) as pool:
        for done in [pool.submit(fetch_submodule, dest, path) for path in SUBMODULES]:
            done.result()
    if app_version:
        stamp_app_version(dest, app_version)
    stamp.write_text(want + '\n')
    return dest


def read_lock() -> dict:
    return json.loads(LOCK.read_text()) if LOCK.exists() else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dest', type=Path)
    parser.add_argument('--repo')
    parser.add_argument('--commit')
    parser.add_argument('--app-version', help="the core's Online::APP_VERSION (default: the lock's appVersion)")
    args = parser.parse_args()
    lock = read_lock()
    repo = args.repo or lock.get('repo')
    commit = args.commit or lock.get('commit')
    app_version = args.app_version or (lock.get('appVersion') if not args.commit else None)
    if not repo or not commit:
        sys.exit(f'No repository/commit: pass --repo and --commit, or write {LOCK.name}')
    if app_version and not VERSION.match(app_version):
        sys.exit(f'Bad appVersion {app_version!r}: major.minor.patch')
    print(fetch(args.dest.resolve(), repo, commit, app_version))


if __name__ == '__main__':
    main()
