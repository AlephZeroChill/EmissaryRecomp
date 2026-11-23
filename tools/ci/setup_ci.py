#!/usr/bin/env python3
"""Sets up a repository's Release workflow on GitHub's runners (docs/releases.md, "CI"), once, with `gh` logged in as the
owner of both repositories. It prints what it would do; --apply does it.

    python tools/ci/setup_ci.py --ci-repo <owner>/EmissaryRecomp --game-repo <owner>/emissary-game-code \
        --branch emissary-brawlonline [--from <ref>] --game <extracted disc DATA> --key-file <private file> [--apply]

1. The CI repository: a fork of kalomaze/EmissaryRecomp (made when missing), Actions on, `--branch` pushed from this
   checkout (`--from`, default HEAD) and made its default branch (tag runs and other branches start from its build caches).
2. The game repository: private (made when missing; refused if it is public), holding game-code.tar.gz.gpg, the
   disc's sys/main.dol and files/module/*.rel encrypted with the key in --key-file (made when missing: keep that file
   private, it is the only copy outside GitHub's secrets).
3. A new read-only deploy key on the game repository; its private half only goes to the CI repository's secret.
4. The CI repository's environment `release`, deployable from `--branch` and tags v* only, with the secrets
   EMISSARY_GAME_KEY and EMISSARY_GAME_DEPLOY_KEY, and the variable EMISSARY_GAME_REPO.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = 'kalomaze/EmissaryRecomp'
GAME_FILE = 'game-code.tar.gz.gpg'


class Setup:
    def __init__(self, args):
        self.args = args
        self.apply = args.apply

    def say(self, text):
        print(('' if self.apply else '[dry run] ') + text, flush=True)

    def gh(self, *args, input=None, check=True, quiet=False) -> str:
        result = subprocess.run(['gh', *args], input=input, capture_output=True, text=True)
        if check and result.returncode != 0:
            raise RuntimeError(f'gh {" ".join(args[:3])}...: {result.stderr.strip()}')
        return result.stdout.strip() if result.returncode == 0 else ''

    def do(self, text, *gh_args, input=None):
        self.say(text)
        if self.apply:
            return self.gh(*gh_args, input=input)
        return ''

    def repo(self, name) -> dict | None:
        out = self.gh('api', f'repos/{name}', check=False)
        return json.loads(out) if out else None

    def ci_repo(self):
        a = self.args
        info = self.repo(a.ci_repo)
        if info is None:
            owner, name = a.ci_repo.split('/')
            self.do(f'fork {UPSTREAM} as {a.ci_repo} (public, as forks of public repositories are)',
                    'repo', 'fork', UPSTREAM, '--clone=false', '--fork-name', name,
                    *([] if owner == self.gh('api', 'user', '--jq', '.login') else ['--org', owner]))
        elif info.get('private'):
            raise RuntimeError(f'{a.ci_repo} is private: GitHub-hosted runners of a private repository are 2-core and '
                               'metered; the release workflow is meant for a public one')
        elif not info.get('permissions', {}).get('admin'):
            raise RuntimeError(f'{a.ci_repo}: not an admin of it (secrets and environments need one)')
        self.do(f'turn Actions on in {a.ci_repo}', 'api', '-X', 'PUT', f'repos/{a.ci_repo}/actions/permissions',
                '-F', 'enabled=true', '-f', 'allowed_actions=all')
        self.say(f'push {a.source} to {a.ci_repo} as {a.branch}')
        if self.apply:
            subprocess.run(['git', '-C', str(ROOT), 'push', f'git@github.com:{a.ci_repo}.git',
                            f'{a.source}:refs/heads/{a.branch}'], check=True)
        self.do(f'make {a.branch} the default branch of {a.ci_repo}', 'api', '-X', 'PATCH', f'repos/{a.ci_repo}',
                '-f', f'default_branch={a.branch}')

    def game_repo(self):
        a = self.args
        info = self.repo(a.game_repo)
        if info is None:
            self.do(f'create the private repository {a.game_repo}', 'repo', 'create', a.game_repo, '--private',
                    '--description', 'EmissaryRecomp CI: the game code, encrypted. Private: never make it public.')
        elif not info.get('private'):
            raise RuntimeError(f'{a.game_repo} is public: the game code goes only into a private repository')
        key = Path(a.key_file)
        if not key.exists():
            self.say(f'make a new key in {key} (keep it private)')
            if self.apply:
                run_tool('keygen', '--key-file', key)
        self.say(f'encrypt {a.game} (sys/main.dol, files/module/*.rel) and push it to {a.game_repo} as {GAME_FILE}')
        if not self.apply:
            return
        with tempfile.TemporaryDirectory(prefix='emissary-game-repo-') as work:
            work = Path(work)
            run_tool('pack', '--game', a.game, '--out', work / GAME_FILE, '--key-file', key)
            (work / 'README.md').write_text(
                'The game code EmissaryRecomp\'s release workflow builds from, encrypted '
                '(tools/ci/game_code.py in EmissaryRecomp). Keep this repository private.\n', encoding='utf-8')
            git = ['git', '-C', str(work)]
            subprocess.run([*git, 'init', '-q', '-b', 'main'], check=True)
            subprocess.run([*git, 'add', GAME_FILE, 'README.md'], check=True)
            subprocess.run([*git, 'commit', '-q', '-m', 'The game code, encrypted'], check=True)
            subprocess.run([*git, 'push', '-q', '--force', f'git@github.com:{a.game_repo}.git', 'main'], check=True)

    def deploy_key(self) -> str | None:
        a = self.args
        self.say(f'make a read-only deploy key for {a.game_repo} (its private half only goes into the secret)')
        if not self.apply:
            return None
        with tempfile.TemporaryDirectory(prefix='emissary-deploy-key-') as work:
            path = Path(work) / 'key'
            subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-C', f'{a.ci_repo} release workflow',
                            '-f', str(path)], check=True)
            title = f'{a.ci_repo} release workflow'
            for old in json.loads(self.gh('api', f'repos/{a.game_repo}/keys')):
                if old.get('title') == title:
                    self.gh('api', '-X', 'DELETE', f'repos/{a.game_repo}/keys/{old["id"]}')
            self.gh('api', '-X', 'POST', f'repos/{a.game_repo}/keys', '-f', f'title={title}',
                    '-f', f'key={path.with_suffix(".pub").read_text().strip()}', '-F', 'read_only=true')
            return path.read_text()

    def environment(self, private_key: str | None):
        a = self.args
        env = 'release'
        self.do(f'environment {env} in {a.ci_repo}: deployable from {a.branch} and tags v* only',
                'api', '-X', 'PUT', f'repos/{a.ci_repo}/environments/{env}', '--input', '-',
                input=json.dumps({'deployment_branch_policy': {'protected_branches': False,
                                                               'custom_branch_policies': True}}))
        policies = f'repos/{a.ci_repo}/environments/{env}/deployment-branch-policies'
        existing = json.loads(self.gh('api', policies, check=False) or '{}').get('branch_policies', [])
        for name, kind in ((a.branch, 'branch'), ('v*', 'tag')):
            if not any(p.get('name') == name and p.get('type', 'branch') == kind for p in existing):
                self.do(f'  allow {kind} {name}', 'api', '-X', 'POST', policies, '-f', f'name={name}',
                        '-f', f'type={kind}')
        self.say(f'secrets EMISSARY_GAME_KEY, EMISSARY_GAME_DEPLOY_KEY and variable EMISSARY_GAME_REPO={a.game_repo}')
        if self.apply:
            self.gh('secret', 'set', 'EMISSARY_GAME_KEY', '--repo', a.ci_repo, '--env', env,
                    input=Path(a.key_file).read_text(encoding='utf-8').strip())
            self.gh('secret', 'set', 'EMISSARY_GAME_DEPLOY_KEY', '--repo', a.ci_repo, '--env', env,
                    input=private_key)
            self.gh('variable', 'set', 'EMISSARY_GAME_REPO', '--repo', a.ci_repo, '--body', a.game_repo)


def run_tool(*args):
    subprocess.run([sys.executable, str(ROOT / 'tools/ci/game_code.py'), *map(str, args)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ci-repo', required=True, help='owner/name: the public repository that runs the releases')
    parser.add_argument('--game-repo', required=True, help='owner/name: the private repository of the game code')
    parser.add_argument('--branch', required=True, help="the CI repository's release branch")
    parser.add_argument('--from', dest='source', default='HEAD', help='what of this checkout to push as --branch')
    parser.add_argument('--game', type=Path, required=True, help='the extracted disc (DATA)')
    parser.add_argument('--key-file', required=True, help='the encryption key (made when missing; keep it private)')
    parser.add_argument('--apply', action='store_true', help='do it (default: say what would be done)')
    args = parser.parse_args()
    if not (args.game / 'sys/main.dol').is_file():
        parser.error(f'--game {args.game}: no sys/main.dol')
    setup = Setup(args)
    setup.ci_repo()
    setup.game_repo()
    setup.environment(setup.deploy_key())
    setup.say(f'done: Actions > Release > Run workflow in https://github.com/{args.ci_repo}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'setup_ci: {error}')
