#!/usr/bin/env python3
"""The game's code for CI, encrypted (docs/releases.md, "CI"): the build translates Brawl's `sys/main.dol` and
`files/module/*.rel` (25 MB of the disc, nothing else), which must never be public. They travel as one file encrypted
with a key only the release jobs have (gpg, AES-256), and the build caches CI keeps between runs (translated game code
and its objects) are encrypted with the same key.

    python tools/ci/game_code.py keygen --key-file <file>                     a new key (keep it private)
    python tools/ci/game_code.py pack --game <DATA> --out game-code.tar.gz.gpg --key-file <file>
    python tools/ci/game_code.py unpack --in game-code.tar.gz.gpg --out <DATA>      (key: EMISSARY_GAME_KEY)
    python tools/ci/game_code.py cache-save --dir build/linux --out build-linux.tar.gz.gpg
    python tools/ci/game_code.py cache-restore --in build-linux.tar.gz.gpg --dir build/linux
    python tools/ci/game_code.py pack-variants --spec <dir>/spec.txt --out pplus-variants.tar.gz.gpg
    python tools/ci/game_code.py unpack-variants --in pplus-variants.tar.gz.gpg --out <dir>

Project+'s code-variant spec (tools/pplus_variants.py; docs/performance.md) travels the same way: its region dumps are
P+'s code lists and Brawl Online's plugin as they sit in the game's memory, which stay out of this public repository.

The key is EMISSARY_GAME_KEY or --key-file. gpg is GnuPG 2 (Ubuntu's, or Git for Windows' usr/bin/gpg.exe); it runs
with a throwaway home folder, so no key ring is touched and nothing is cached.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile

MANIFEST = 'game-code.json'  # in the bundle: each file's sha256, checked when unpacked


def find_gpg() -> str:
    found = shutil.which('gpg')
    if not found and os.name == 'nt':
        bundled = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Git/usr/bin/gpg.exe'
        found = str(bundled) if bundled.is_file() else None
    if not found:
        raise RuntimeError('gpg not found (GnuPG 2: apt install gnupg, or Git for Windows)')
    return found


def read_key(args) -> str:
    key = os.environ.get('EMISSARY_GAME_KEY', '').strip()
    if getattr(args, 'key_file', None):
        key = Path(args.key_file).read_text(encoding='utf-8').strip()
    if len(key) < 32:
        raise RuntimeError('no key: set EMISSARY_GAME_KEY or pass --key-file (python tools/ci/game_code.py keygen)')
    return key


class Gpg:
    """gpg --symmetric / --decrypt as a stream, the passphrase in a file only this process can read."""

    def __init__(self, key: str):
        self.home = Path(tempfile.mkdtemp(prefix='emissary-gpg-'))
        os.chmod(self.home, 0o700)
        self.passphrase = self.home / 'passphrase'
        self.passphrase.write_text(key, encoding='utf-8')
        os.chmod(self.passphrase, 0o600)
        self.gpg = find_gpg()

    def path(self, path: Path) -> str:
        # Git for Windows' gpg is an MSYS program: it takes C:/x for a relative path, /c/x is what it means.
        p = Path(path).resolve()
        if os.name == 'nt' and Path(self.gpg).parent.parts[-2:] == ('usr', 'bin') and p.drive.endswith(':'):
            return '/' + p.drive[0].lower() + '/' + '/'.join(p.parts[1:])
        return p.as_posix()

    def command(self, *args) -> list[str]:
        return [self.gpg, '--homedir', self.path(self.home), '--batch', '--yes', '--quiet', '--no-symkey-cache',
                '--pinentry-mode', 'loopback', '--passphrase-file', self.path(self.passphrase), *args]

    def encrypt(self, out: Path) -> subprocess.Popen:
        # The data is compressed before it gets here (gzip); compressing again would only cost time.
        return subprocess.Popen(self.command('--symmetric', '--cipher-algo', 'AES256', '--compress-algo', 'none',
                                             '--output', self.path(out)), stdin=subprocess.PIPE)

    def decrypt(self, path: Path) -> subprocess.Popen:
        return subprocess.Popen(self.command('--decrypt', self.path(path)), stdout=subprocess.PIPE)

    def close(self):
        shutil.rmtree(self.home, ignore_errors=True)


def finish(process: subprocess.Popen, what: str):
    for stream in (process.stdin, process.stdout):
        if stream:
            stream.close()
    if process.wait() != 0:
        raise RuntimeError(f'gpg could not {what} (exit {process.returncode}): a wrong key, or a damaged file')


class reading:
    """Around reading gpg's output: a wrong key or a damaged file reads as a broken archive; say which it was."""

    def __init__(self, process: subprocess.Popen):
        self.process = process

    def __enter__(self):
        return self

    def __exit__(self, kind, error, _trace):
        if error is not None and isinstance(error, (tarfile.TarError, OSError, EOFError)):
            self.process.stdout.close()
            if self.process.wait() != 0:
                raise RuntimeError(f'gpg could not decrypt (exit {self.process.returncode}): a wrong key, '
                                   'or a damaged file') from error
        return False


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def game_files(game: Path) -> list[tuple[str, Path]]:
    dol = game / 'sys/main.dol'
    modules = game / 'files/module'
    if not dol.is_file() or not modules.is_dir():
        raise RuntimeError(f'{game}: not an extracted disc (DATA with sys/main.dol and files/module/)')
    files = [('sys/main.dol', dol)]
    files += [(f'files/module/{p.name}', p) for p in sorted(modules.iterdir()) if p.is_file()]
    return files


def plain_info(name: str, size: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = size, 0o644, 0  # the same bytes every time the same disc is packed
    return info


def keygen(args):
    path = Path(args.key_file)
    if path.exists():
        raise RuntimeError(f'{path} exists: a new key would make the files encrypted with it unreadable')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(secrets.token_urlsafe(48) + '\n', encoding='utf-8')
    print(f'Wrote a new key to {path}. Keep it private: it is the GitHub secret EMISSARY_GAME_KEY.')


def pack(args):
    files = game_files(args.game)
    manifest = {'files': {}}
    gpg = Gpg(read_key(args))
    try:
        process = gpg.encrypt(args.out)
        with tarfile.open(fileobj=process.stdin, mode='w|gz', compresslevel=9) as tar:
            for name, path in files:
                data = path.read_bytes()
                manifest['files'][name] = sha256(data)
                tar.addfile(plain_info(name, len(data)), io.BytesIO(data))
            text = (json.dumps(manifest, indent=1, sort_keys=True) + '\n').encode()
            tar.addfile(plain_info(MANIFEST, len(text)), io.BytesIO(text))
        finish(process, 'encrypt')
    finally:
        gpg.close()
    total = sum(p.stat().st_size for _, p in files)
    print(f'{args.out}: {len(files)} files ({total / 1e6:.1f} MB; main.dol {manifest["files"]["sys/main.dol"][:16]}), '
          f'encrypted ({args.out.stat().st_size / 1e6:.1f} MB)')


def unpack(args):
    out = args.out
    if out.exists() and any(out.iterdir()):
        raise RuntimeError(f'{out} is not empty')
    out.mkdir(parents=True, exist_ok=True)
    gpg = Gpg(read_key(args))
    seen, manifest = {}, None
    try:
        process = gpg.decrypt(args.input)
        with reading(process), tarfile.open(fileobj=process.stdout, mode='r|gz') as tar:
            for member in tar:
                name = PurePosixPath(member.name)
                if not member.isfile() or name.is_absolute() or '..' in name.parts:
                    raise RuntimeError(f'unexpected entry {member.name!r} in {args.input}')
                data = tar.extractfile(member).read()
                if member.name == MANIFEST:
                    manifest = json.loads(data)
                    continue
                if not (member.name == 'sys/main.dol' or (name.parent == PurePosixPath('files/module')
                                                         and name.suffix == '.rel')):
                    raise RuntimeError(f'unexpected file {member.name!r} in {args.input}')
                target = out / member.name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                seen[member.name] = sha256(data)
        finish(process, 'decrypt')
    finally:
        gpg.close()
    if manifest is None or seen != manifest.get('files'):
        raise RuntimeError(f'{args.input}: the files do not match its manifest')
    print(f'{out}: {len(seen)} files (main.dol {seen["sys/main.dol"][:16]})')


def variant_files(spec: Path) -> list[tuple[str, Path]]:
    """A code-variant spec (projects/brawl/tools/gen_variant_spec.py): spec.txt and the region dumps it names."""
    if spec.name != 'spec.txt' or not spec.is_file():
        raise RuntimeError(f'{spec}: not a code-variant spec (spec.txt)')
    files = [('spec.txt', spec)]
    files += [(p.name, p) for p in sorted(spec.parent.glob('region_*.bin'))]
    return files


def variant_name_ok(name: str) -> bool:
    return name == 'spec.txt' or (name.startswith('region_') and name.endswith('.bin') and '/' not in name
                                  and len(name) == len('region_00000000.bin'))


def pack_variants(args):
    files = variant_files(args.spec)
    manifest = {'files': {}}
    gpg = Gpg(read_key(args))
    try:
        # The archive goes to gpg as a file in its throwaway home, not through a pipe: Git for Windows' gpg sometimes
        # never exited after a few hundred KB on stdin while it started its agent (the 25 MB game code never did).
        plain = gpg.home / 'variants.tar.gz'
        with tarfile.open(plain, mode='w:gz', compresslevel=9) as tar:
            for name, path in files:
                data = path.read_bytes()
                manifest['files'][name] = sha256(data)
                tar.addfile(plain_info(name, len(data)), io.BytesIO(data))
            text = (json.dumps(manifest, indent=1, sort_keys=True) + '\n').encode()
            tar.addfile(plain_info(MANIFEST, len(text)), io.BytesIO(text))
        subprocess.run(gpg.command('--symmetric', '--cipher-algo', 'AES256', '--compress-algo', 'none',
                                   '--output', gpg.path(args.out), gpg.path(plain)), check=True,
                       stdin=subprocess.DEVNULL, timeout=120)
    finally:
        gpg.close()
    print(f'{args.out}: {len(files)} files (spec.txt {manifest["files"]["spec.txt"][:16]}), '
          f'encrypted ({args.out.stat().st_size / 1e3:.0f} KB)')


def unpack_variants(args):
    out = args.out
    if out.exists() and any(out.iterdir()):
        raise RuntimeError(f'{out} is not empty')
    out.mkdir(parents=True, exist_ok=True)
    gpg = Gpg(read_key(args))
    seen, manifest = {}, None
    try:
        process = gpg.decrypt(args.input)
        with reading(process), tarfile.open(fileobj=process.stdout, mode='r|gz') as tar:
            for member in tar:
                if not member.isfile() or not (member.name == MANIFEST or variant_name_ok(member.name)):
                    raise RuntimeError(f'unexpected entry {member.name!r} in {args.input}')
                data = tar.extractfile(member).read()
                if member.name == MANIFEST:
                    manifest = json.loads(data)
                    continue
                (out / member.name).write_bytes(data)
                seen[member.name] = sha256(data)
        finish(process, 'decrypt')
    finally:
        gpg.close()
    if manifest is None or seen != manifest.get('files') or 'spec.txt' not in seen:
        raise RuntimeError(f'{args.input}: the files do not match its manifest')
    print(f'{out / "spec.txt"}: {len(seen)} files (spec.txt {seen["spec.txt"][:16]})')


def cache_save(args):
    """The build folder (translated code, objects, the fetched core) as one encrypted file, times kept: Ninja builds
    only what changed once it is restored at the same path."""
    gpg = Gpg(read_key(args))
    try:
        process = gpg.encrypt(args.out)
        skip = {Path(args.dir, s).resolve() for s in args.skip}
        with tarfile.open(fileobj=process.stdin, mode='w|gz', compresslevel=1) as tar:
            def keep(info):
                return None if Path(args.dir, info.name).resolve() in skip else info
            for entry in sorted(Path(args.dir).iterdir()):
                tar.add(entry, arcname=entry.name, filter=keep)
        finish(process, 'encrypt')
    finally:
        gpg.close()
    print(f'{args.out}: {args.out.stat().st_size / 1e6:.0f} MB')


def cache_restore(args):
    gpg = Gpg(read_key(args))
    args.dir.mkdir(parents=True, exist_ok=True)
    try:
        process = gpg.decrypt(args.input)
        with reading(process), tarfile.open(fileobj=process.stdout, mode='r|gz') as tar:
            tar.extractall(args.dir, filter='tar')  # our own file: links and times as they were, nothing outside
        finish(process, 'decrypt')
    finally:
        gpg.close()
    print(f'{args.dir}: restored from {args.input}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('keygen')
    p.add_argument('--key-file', required=True)
    p = sub.add_parser('pack')
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--key-file')
    p = sub.add_parser('unpack')
    p.add_argument('--in', dest='input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--key-file')
    p = sub.add_parser('pack-variants')
    p.add_argument('--spec', type=Path, required=True, help="the spec's spec.txt (its region_*.bin beside it)")
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--key-file')
    p = sub.add_parser('unpack-variants')
    p.add_argument('--in', dest='input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--key-file')
    p = sub.add_parser('cache-save')
    p.add_argument('--dir', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--skip', action='append', default=[], help='a path under --dir to leave out (repeatable)')
    p.add_argument('--key-file')
    p = sub.add_parser('cache-restore')
    p.add_argument('--in', dest='input', type=Path, required=True)
    p.add_argument('--dir', type=Path, required=True)
    p.add_argument('--key-file')
    args = parser.parse_args()
    {'keygen': keygen, 'pack': pack, 'unpack': unpack, 'pack-variants': pack_variants,
     'unpack-variants': unpack_variants, 'cache-save': cache_save, 'cache-restore': cache_restore}[args.command](args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired, tarfile.TarError, KeyError,
            ValueError) as error:
        raise SystemExit(f'game_code: {error}')
