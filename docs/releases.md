# Releases with Brawl Online's online core

EmissaryRecomp's releases are what Brawl Online's launcher installs when a player picks "Play Project+ with
EmissaryRecomp" (brawlonline.net, `launcher/src/dolphin/emissary/`): the recompiled game with Brawl Online's online
core linked in (`-DBRAWL_ONLINE=lock`), so it plays Brawl Online's games (Ranked, rooms, Direct) against Dolphin
players. This page says what a release is, how one is made (`tools/release.py`, locally or by CI) and what CI needs.

## Version and tag

- `VERSION` holds the release's version, `major.minor.patch` (`0.7.0`). Bump it in the commit to release.
- The release is the git tag and GitHub release `v<VERSION>` (`v0.7.0`), or `v<VERSION>-<suffix>` (`v0.7.0-rc.1`) for a
  prerelease. A GitHub prerelease is never `/releases/latest`, which is what the launcher installs and updates to, so
  no player gets one unless the launcher is pointed at it (its test switch `PPO_EMISSARY_TAG=<tag>`).
- The build knows its tag (`RELEASE_VERSION`, compiled in as `MKW_RELEASE_VERSION`): the core logs it at start
  ("Brawl Online core in EmissaryRecomp v0.7.0"), and once the core has `Engine::Identity()` (brawlonline.net
  `docs/design/brawl-online-core.md` §10, "Which engine") the recomp reports `{"emissary", "v0.7.0"}` to the servers.

## The core it links: `brawl_online.lock`

```json
{
  "repo": "https://github.com/sarahkittyy/brawlonline.net.git",
  "commit": "747a9a574a5e1e6c25338fa62a3c07630dacf2c6",
  "appVersion": "0.2.3",
  "protocol": 1
}
```

- `repo`, `commit`: the brawlonline.net commit whose `core/` (and the parts of Dolphin it builds from) is linked.
  `tools/fetch_brawl_online.py` fetches only those (a sparse, blob-less clone plus each needed submodule's one pinned
  commit: about 3 minutes and 125 MB) into `build/brawl-online`, shared by the build folders. The commit must be on
  a branch of that repository (`emissary-brawlonline`); never push its `main` for this (pushing `main` deploys Brawl
  Online).
- `appVersion`: the Brawl Online release that commit belongs to (Brawl Online's CI numbers its builds `0.2.<run>`; the
  core at a commit is the release of the last `client.yml` run whose commit it contains). The fetch writes it into the
  core's `Online::APP_VERSION`, which the game sends to the servers as `appVersion`, as Brawl Online's CI does for its
  own builds (the repository says `0.1.0`), and the build passes it as `BRAWL_ONLINE_APP_VERSION` too. The servers hold
  a game to its platform's newest release (and refuse older ones once required), so a recomp release plays on the
  servers only while its core is a release they accept: moving the lock to a newer core means a new recomp release.
- `protocol`: what the release needs from the launcher (the launch set-up: `BRAWL_ONLINE_CORE`'s user folder, the
  hand-off files, the card and codes): the launcher's `EMISSARY_PROTOCOL`. The launcher does not play a release whose
  manifest names another one (Dolphin plays instead). Bump both together when that contract changes.

## The downloads

Each release has these assets:

| Asset | What it is |
|---|---|
| `EmissaryRecomp-<tag>-Windows.zip` | Windows x86-64 |
| `EmissaryRecomp-<tag>-Linux.tar.gz` | Linux x86-64, glibc 2.38+ (Ubuntu 24.04, Debian 13, SteamOS 3.6, Fedora 39, Arch...) |
| `EmissaryRecomp-<tag>-macOS.tar.gz` | macOS 12+ on Apple silicon (arm64); only when built on a Mac (`--platforms macos`) |
| `manifest.json` | what the release is, and the archives' sizes and sha256 |
| `SHA256SUMS` | the sha256 of the other assets, for `sha256sum -c` |

Each archive has one top folder, `EmissaryRecomp/`, with the program at its top (the launcher looks for
`WiiCompiled[.exe]` in the archive's top folder, removes any `portable.txt` and `UserData` it finds beside it, and keeps
the player's settings, saves and logs above the version folders):

```
EmissaryRecomp/
  WiiCompiled.exe        Windows: the loader; it starts engine_usa.dll, or engine_usa_v2.dll on a CPU without AVX2/FMA
  engine_usa.dll         the game (x86-64-v3)                                       (Windows)
  engine_usa_v2.dll      the game for older CPUs (x86-64-v2)                        (Windows)
  SDL3.dll, libc++.dll, libunwind.dll, libpng16.dll, libz.dll                      (Windows; Windows' own UCRT)
  WiiCompiled            Linux: the program (RUNPATH $ORIGIN)                        (Linux)
  lib*.so*               any library the build made itself (none today: it needs only libz, glibc, libstdc++) (Linux)
  WiiCompiled            macOS: the program (rpath @executable_path, signed ad hoc)  (macOS)
  lib*.dylib             any library the build made itself (libpng, when it is shared) (macOS)
  cacert.pem             TLS roots for the game's own HTTPS (Schannel on Windows)    (Linux, macOS)
  dsp_coef.bin           the Wii DSP coefficient ROM
  wii_bootstrap/         the Wii's first-run NAND files
  Sys/Rollback/          Brawl Online's rollback region sets (gp-v*.json), read beside the program
  Sys/Resources/         the in-game UI's fonts, and Emissary's title card (emissary_mask.png, emissary_wordmark.png)
  startup/smash_ball.png the title card's art
  release.json           the manifest without the assets (what this folder is)
  README.txt, LICENSE.txt, CREDITS.txt, licenses/
```

No disc data, settings, saves, SD card or `portable.txt` is in a download (`tools/release.py` refuses to package one):
the player's own disc is extracted by the launcher, and the game reads it through `UserData/Config.toml`'s
`dvd_root`.

### manifest.json

```json
{
  "version": "v0.7.0-rc.1",
  "commit": "<the EmissaryRecomp commit built>",
  "brawlOnline": "0.2.3",
  "protocol": 1,
  "core": {
    "repo": "https://github.com/sarahkittyy/brawlonline.net.git",
    "commit": "747a9a574a5e1e6c25338fa62a3c07630dacf2c6",
    "appVersion": "0.2.3"
  },
  "engine": { "name": "emissary", "version": "v0.7.0-rc.1" },
  "prerelease": true,
  "platforms": {
    "windows": { "asset": "EmissaryRecomp-v0.7.0-rc.1-Windows.zip", "size": 0, "sha256": "...",
                 "program": "EmissaryRecomp/WiiCompiled.exe" },
    "linux": { "asset": "EmissaryRecomp-v0.7.0-rc.1-Linux.tar.gz", "size": 0, "sha256": "...",
               "program": "EmissaryRecomp/WiiCompiled" }
  }
}
```

`version` is the tag (the launcher compares it with the release's), `brawlOnline` the core's Brawl Online release
(its `appVersion`, a plain string the launcher shows and compares), `protocol` as above; `core` names the exact
commit. The launcher reads `version`, `brawlOnline` and `protocol`; the rest is for people and tools.

## Making a release

Releases are built by CI on GitHub's own runners (below): Actions > Release > Run workflow on the release branch, or a
pushed tag. `tools/release.py` builds the same release on a PC, from a clean checkout of the commit to release:

```powershell
python tools\release.py --game D:\path\to\extracted\DATA --prerelease rc.1            # build + package, in dist\<tag>\
python tools\release.py --game D:\path\to\extracted\DATA --prerelease rc.1 --publish  # and the GitHub release
```

1. The Linux program: `tools/release-linux.sh` in the WSL distro `EmissaryRecomp-Build` (below) clones the commit into
   `~/emissary-release` (a build on `/mnt/<drive>` is many times slower), builds it with the core from the lock and
   copies the program, the libraries the build made itself (with RUNPATH `$ORIGIN`), `dsp_coef.bin`, `cacert.pem`,
   `wii_bootstrap/` and `Sys/` to `dist/<tag>/linux-native/`. It checks that the program finds every library. (CI
   builds its own checkout in place: `EMISSARY_RELEASE_IN_PLACE=1`.)
2. Both Windows engines: `tools/build-brawl.ps1 -NoPlay -BrawlOnline lock -ReleaseVersion <tag>` (`build/release`,
   engine_usa.dll and the loader), then with `-OldCpu` (`build/release-v2`, engine_usa_v2.dll).
3. The macOS program (`--platforms ...,macos`, on an Apple silicon Mac only): `tools/release-macos.sh` builds the
   commit the same way in `~/emissary-release-macos` and copies it to `dist/<tag>/macos-native/`, with the libraries
   the build made itself beside it (rpath `@executable_path`), refusing any library outside `/usr/lib` and `/System`
   (Homebrew's and XQuartz's, which `tools/build.py` keeps out of a macOS build), signed ad hoc. A release with Windows
   too is packaged on the Windows machine: copy `macos-native/` into its `dist/<tag>/` and run with `--skip-build`.
   (`EMISSARY_RELEASE_CORE=<checkout>` links that core instead of the lock's: Brawl Online's staging.)
4. The archives, `manifest.json` and `SHA256SUMS` in `dist/<tag>/`.
5. With `--publish`: `gh release create <tag>` at the commit (which must be pushed), a prerelease for a suffixed tag. A
   release without a suffix is published without becoming `/releases/latest`, which is what every launcher installs:
   `--latest` makes it the latest, or later `gh release edit <tag> --latest`.

Builds are incremental: only what changed is translated or compiled again. A full build is short: translating the
game takes about a minute, the Linux program compiled from scratch in 10 minutes with 6 jobs and a Windows engine in
10 minutes with 4 jobs, on an 8-core PC (2026-11-23). `--skip-build` packages the builds already there (also on Linux, from Windows builds copied in: CI's
last job); `--platforms windows` or `linux` makes one; `--allow-dirty` builds uncommitted changes for a local try
(never published). `dist/` is also a feed for the launcher's test switch `PPO_EMISSARY_RELEASES=<repo>\dist`
(`<dir>/<tag>/` with the assets).

To try a download without the launcher, the way Brawl Online's harness starts the recomp
(brawlonline.net `harness/ppharness/recomp.py`): unpack it, write `EmissaryRecomp/portable.txt` and
`EmissaryRecomp/UserData/Config.toml` with `[paths] dvd_root = "<extracted DATA>"`, and point the harness at the folder
(`PPHARNESS_RECOMP=<folder>`).

### The Linux build machine (WSL)

Ubuntu 24.04 in WSL, made from Ubuntu's own WSL image and used only for this (any user of the PC can have it; a
Windows service account cannot see a user's WSL distros):

```powershell
curl.exe -LO https://cloud-images.ubuntu.com/wsl/releases/24.04/current/ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz
# check it against https://cloud-images.ubuntu.com/wsl/releases/24.04/current/SHA256SUMS
wsl --import EmissaryRecomp-Build C:\wsl\EmissaryRecomp-Build ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz --version 2
wsl -d EmissaryRecomp-Build -u root -- bash tools/linux-build-deps.sh
```

The release's glibc floor is the build system's: building on a newer distro would make the program refuse to start on
Ubuntu 24.04 and SteamOS. Remove it with `wsl --unregister EmissaryRecomp-Build`.

The Linux program plays Brawl Online's harness tests from Windows in this distro (brawlonline.net
`harness/ppharness/recomp.py`, `PPHARNESS_RECOMP_WSL=EmissaryRecomp-Build:<its build/native>`): against Windows
Dolphin or a Windows recomp, it needs WSL's mirrored networking (`%USERPROFILE%\.wslconfig`, `[wsl2]`
`networkingMode=mirrored`, then `wsl --shutdown`, which stops every distro), so the servers, the harness and the other
game reach it on 127.0.0.1. WSL renders with llvmpipe (software Vulkan): the game runs slowly there (give the harness
`PPHARNESS_TIMEOUT_SCALE=6` and `PPHARNESS_RECOMP_TOML=video.resolution_multiplier=0.25`); judge a set by its
checksums. 2026-11-23, the Linux program in WSL (3 processors) played
`test_direct_set_under_the_gameplay_session` against Windows Dolphin (3 games, 7,446 confirmed checksums) and against
the Windows recomp (6,958), 0 mismatches.

## CI: `.github/workflows/release.yml`

Builds a release on GitHub's own runners, so a release needs no one's PC: the Linux program on `ubuntu-24.04` (the
glibc floor above), each Windows engine on `windows-2025`, the three at once, then one job packages them with
`tools/release.py --skip-build` and publishes the release. It starts only when someone with write access to the
repository starts it: a pushed tag `v*.*.*` (built and published), or Actions > Release > Run workflow on the release
branch (a prerelease suffix, and whether to publish; unpublished files stay a workflow artifact for 7 days). It has no
`pull_request` or `pull_request_target` trigger, and none must ever be added to it: its jobs decrypt the game's code.

### The game's code

The build translates Brawl's `sys/main.dol` and `files/module/*.rel`: 127 files, 25 MB of the disc, nothing else
(`tools/build.py`). Those are copyrighted and never go anywhere public, so CI gets them this way:

- `tools/ci/game_code.py pack` makes one file of them, `game-code.tar.gz.gpg` (7.3 MB), encrypted with a random key
  (gpg, AES-256; GnuPG is on both runners: Ubuntu's, and Git for Windows' on the Windows ones).
- That file is in a **private** repository (`EMISSARY_GAME_REPO`), which the jobs clone with a read-only deploy key
  of that one repository.
- The key and the deploy key's private half are secrets of the CI repository. Each build job decrypts the files into the runner's temporary folder and deletes them when it
  ends (a GitHub runner is a new machine for every job anyway).
- What is uploaded is only the program, the same files a release has. The build caches (below) are the game's code too
  (translated, compiled), so they are encrypted with the same key.

Anyone who can run the workflow with the secrets can read the game's files: that is whoever has write access to the
CI repository, and the code of the commit built (it could copy the decrypted files
anywhere). So only build commits whose changes you know. GitHub masks the secrets in the logs, not files made with them;
the workflow never prints the files, but a compile error in translated code would show that line of it in a log, and in
a public repository the logs and workflow artifacts can be seen by anyone (artifacts by anyone signed in to GitHub).

A new key: `game_code.py keygen` and pack again (and the secret updated); old caches then fail to
decrypt and are built again.

### Build caches

Each job keeps its build folder (the translated game, the objects, the fetched core; not the .NET SDK or the downloads)
encrypted in GitHub's Actions cache after a run on a branch, and the next run restores the newest one at the same path,
so Ninja builds only what changed. A cache from the default branch is what tag runs and other branches start from
(caches saved by a tag's run are only that tag's): runs on the release branch keep their own caches, and a tag run whose
branch is not the default one builds from scratch.
GitHub keeps 10 GB of caches per repository and drops one unused for 7 days; the repository variable
`EMISSARY_BUILD_CACHE=off` turns them off. Since a full build takes well under an hour, a cache only saves time.

### Setting it up

Chosen (Sarah, 2026-11-24): option B below, in kalomaze/EmissaryRecomp, its owner setting it up once; everyone with
write access there is trusted, so the secrets are plain repository secrets (no environment or reviewers).

1. A **private** repository (e.g. `kalomaze/emissary-game-code`) with `game-code.tar.gz.gpg` at its top: made from an
   extracted disc with `python tools/ci/game_code.py pack --game <DATA> --out game-code.tar.gz.gpg --key-file <key>`
   (`keygen --key-file <key>` first for a new key; keep that file private).
2. A deploy key for it: `ssh-keygen -t ed25519 -N "" -f deploykey`, and `deploykey.pub` added to the private repository
   (Settings > Deploy keys, without write access).
3. In EmissaryRecomp, Settings > Secrets and variables > Actions: the secrets `EMISSARY_GAME_KEY` (the key file's
   text) and `EMISSARY_GAME_DEPLOY_KEY` (the text of `deploykey`, the private half), and the variable
   `EMISSARY_GAME_REPO` (`owner/name` of the private repository).

Then Actions > Release > Run workflow (the button shows once `.github/workflows/release.yml` is on the default branch),
or push a `v*` tag. Every job is skipped in a repository without `EMISSARY_GAME_REPO` (forks, or this one until it is
set up). Brawl Online's launcher installs releases from the repository its `EMISSARY_REPO` names (brawlonline.net
`launcher/src/dolphin/emissary/types.ts`): kalomaze/EmissaryRecomp.

To let only some people release, move the two secrets into an environment whose deployment rules allow only the release
branch and `v*` tags (and, if wanted, required reviewers), and give the workflow's build job `environment: <name>`.
`tools/ci/setup_ci.py` does steps 1-3 for a repository its `gh` user owns (`--ci-repo`, `--game-repo`, `--game` or a
packed `--game-file`, `--key-file`; it only says what it would do without `--apply`), forking kalomaze/EmissaryRecomp
when the CI repository does not exist.

### Ways to build releases, and what each costs

| | Where | Needs | Risks and costs |
|---|---|---|---|
| **A** | A self-hosted runner on a PC with the disc (the first design) | the PC on; registering the runner (an admin of the repository) | the disc's files never leave that PC; but every collaborator's workflow code runs on it, and a release waits for it and takes its CPU |
| **B** (chosen) | GitHub's runners in kalomaze/EmissaryRecomp, the game's code encrypted in a private repository | its owner adding two secrets and a variable, and the deploy key | as C; no fork, the launcher's repository stays; everyone with write access there can run the workflow and so read the game's files (all trusted) |
| C | GitHub's runners in a public repository Sarah owns (a fork), the game's code encrypted in a private one | the two repositories, the deploy key and the secrets (`setup_ci.py`, as their owner) | free, three platforms at once, about half an hour to an hour from scratch; only Sarah (and what she pushes) can run it; the game's files are on GitHub, encrypted, in a private repository, decrypted on GitHub's runners for each build; the launcher points at the fork's releases |
| D | GitHub's runners in a private repository | as C | logs and artifacts private, but 2-core runners and 2,000 free minutes a month (Windows minutes count twice): a few releases a month at most; players cannot download a private repository's releases, so they would be published elsewhere |
| E | A runner on a home server or VPS | the machine; a Linux runner | one OS (Linux); those machines are already busy. The Windows engines are built with llvm-mingw, which also runs on Linux, so a Linux machine could cross-compile them, but the build (`build-brawl.ps1`, `Prepare-PortableTools.ps1`) is Windows-only today |
| F | macOS (later) | `macos-15` runners (arm64, 3 cores, 7 GB, free in a public repository) | the game, Brawl Online's core and the launcher play on Apple silicon since 2026-11-25 (`tools/release-macos.sh`); CI has no macOS job yet |

The repository variable or the secrets missing, every job is skipped, so the same workflow serves B and C.

## Updating the core

1. Merge or commit the core change on brawlonline.net's `emissary-recomp` branch and push it to its
   `emissary-brawlonline` branch (never `main`).
2. In `brawl_online.lock`: the new `commit`, and `appVersion` = the Brawl Online release that commit contains.
3. Bump `VERSION`, commit, and release. A player's launcher updates to the new `/releases/latest` before its next
   game (never during one, nor between the games of a Ranked set).
