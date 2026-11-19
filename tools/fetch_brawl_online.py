"""Fetches the part of Brawl Online (brawlonline.net) this build links: its online core and what the core's standalone
build uses (core/standalone/CMakeLists.txt: Dolphin's Common, Brawlback's headers, four of Dolphin's rollback files, the
region sets in Data/Sys/Rollback and ten of Dolphin's Externals, seven of them git submodules), at the commit
brawl_online.lock pins. A blob-less sparse clone, so a few tens of MB instead of the whole repository.

    python tools/fetch_brawl_online.py <destination>            # the lock's repository and commit
    python tools/fetch_brawl_online.py <destination> --repo <url or path> --commit <sha>

Prints the checkout's path (BRAWL_ONLINE_ROOT for the native configure). Reuses <destination> when it already holds
the commit.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'brawl_online.lock'

PATHS = [
    'core/',
    'dolphin/Source/Core/Common/',
    'dolphin/Source/Core/Core/Brawlback/include/',
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


def git(*args, cwd=None):
    subprocess.run(['git', *args], cwd=cwd, check=True)


def git_out(*args, cwd=None) -> str:
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def fetch(dest: Path, repo: str, commit: str) -> Path:
    stamp = dest / '.brawl-online-commit'
    if stamp.exists() and stamp.read_text().strip() == commit:
        return dest
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    git('init', '-q', cwd=dest)
    git('remote', 'add', 'origin', repo, cwd=dest)
    git('config', 'core.sparseCheckout', 'true', cwd=dest)
    git('sparse-checkout', 'set', '--no-cone', *PATHS, cwd=dest)
    git('-c', 'protocol.version=2', 'fetch', '-q', '--depth', '1', '--filter=blob:none', 'origin', commit, cwd=dest)
    git('checkout', '-q', 'FETCH_HEAD', cwd=dest)
    # Submodule URLs come from the fetched .gitmodules; only these seven are needed.
    git('submodule', 'update', '--init', '--depth', '1', '--filter=blob:none', '--', *SUBMODULES, cwd=dest)
    stamp.write_text(commit + '\n')
    return dest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dest', type=Path)
    parser.add_argument('--repo')
    parser.add_argument('--commit')
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text()) if LOCK.exists() else {}
    repo = args.repo or lock.get('repo')
    commit = args.commit or lock.get('commit')
    if not repo or not commit:
        sys.exit(f'No repository/commit: pass --repo and --commit, or write {LOCK.name}')
    print(fetch(args.dest.resolve(), repo, commit))


if __name__ == '__main__':
    main()
