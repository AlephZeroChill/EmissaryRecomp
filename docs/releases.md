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
  "commit": "c09f03b80b5df6495b2593cd63c741476b8dded3",
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
  SDL3.dll, webgpu_dawn.dll, dxcompiler.dll, dxil.dll, libc++.dll, libunwind.dll,
  msvcp140*.dll, vcruntime140*.dll                                                  (Windows)
  WiiCompiled            Linux: the program (RUNPATH $ORIGIN)                        (Linux)
  libpng16.so.16         the libraries the build made itself                         (Linux)
  cacert.pem             TLS roots for the game's own HTTPS (Schannel on Windows)    (Linux)
  dsp_coef.bin           the Wii DSP coefficient ROM
  wii_bootstrap/         the Wii's first-run NAND files
  Sys/Rollback/          Brawl Online's rollback region sets (gp-v*.json), read beside the program
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
    "commit": "c09f03b80b5df6495b2593cd63c741476b8dded3",
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

`tools/release.py` does all of it from a clean checkout of the commit to release:

```powershell
python tools\release.py --game D:\path\to\extracted\DATA --prerelease rc.1            # build + package, in dist\<tag>\
python tools\release.py --game D:\path\to\extracted\DATA --prerelease rc.1 --publish  # and the GitHub release
```

1. The Linux program: `tools/release-linux.sh` in the WSL distro `EmissaryRecomp-Build` (below) clones the commit into
   `~/emissary-release` (a build on `/mnt/<drive>` is many times slower), builds it with the core from the lock and
   copies the program, the libraries the build made itself (with RUNPATH `$ORIGIN`), `dsp_coef.bin`, `cacert.pem`,
   `wii_bootstrap/` and `Sys/` to `dist/<tag>/linux-native/`. It checks that the program finds every library.
2. Both Windows engines: `tools/build-brawl.ps1 -NoPlay -BrawlOnline lock -ReleaseVersion <tag>` (`build/release`,
   engine_usa.dll and the loader), then with `-OldCpu` (`build/release-v2`, engine_usa_v2.dll).
3. The archives, `manifest.json` and `SHA256SUMS` in `dist/<tag>/`.
4. With `--publish`: `gh release create <tag>` at the commit (which must be pushed), a prerelease for a suffixed tag.

Builds are incremental: only what changed is translated or compiled again (a full build of one engine translates
the whole game first). `--skip-build` packages the builds already there; `--platforms windows` or `linux` makes one;
`--allow-dirty` builds uncommitted changes for a local try (never published). `dist/` is also a feed for the
launcher's test switch `PPO_EMISSARY_RELEASES=<repo>\dist` (`<dir>/<tag>/` with the assets).

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

## CI: `.github/workflows/release.yml`

Starts only when a maintainer does: a pushed tag `v*.*.*` (builds and publishes that tag) or Actions > Release > Run
workflow (a prerelease suffix and whether to publish; unpublished files stay a workflow artifact for 7 days). Both
need write access to the repository. It has no `pull_request` or `pull_request_target` trigger, and none must ever be
added to a workflow that runs on the release runner.

Setting it up needs a repository admin (registering a runner is an admin's), once:

1. On the release PC (Windows, 8+ cores, 32 GB, ~40 GB free): Python 3.12, Git, PowerShell 7, `gh`; the WSL distro
   above, made by the account the runner runs as; an extracted disc (USA, RSBE01, `DATA` with `sys/main.dol` and
   `files/`).
2. Settings > Actions > Runners > New self-hosted runner (Windows x64): install it with the labels
   `emissary-release` (the workflow asks for `self-hosted, Windows, X64, emissary-release`), running as that
   account, not as a service account. In the runner folder's `.env` file: `EMISSARY_GAME=<path to DATA>`.
3. Settings > Actions > General: "Require approval for all external contributors" for fork pull requests (no
   workflow here runs on them, but one added later could), and keep "Allow GitHub Actions to create and approve
   pull requests" off.

The workflow's `GITHUB_TOKEN` (contents: write) creates the release; no other secret is needed.

### Two ways to run it, and their risks

**A. A self-hosted runner on a PC that has the disc (implemented).**
The disc's files never leave that PC; only the built program is uploaded, as a release. Builds stay incremental
between releases (the runner keeps `build/`, `toolchain/` and the fetched core), so a release that changes a little
takes minutes, not hours. Risks: the runner runs whatever workflow code a commit with write access pushes, as the
runner's user, on that PC, so everyone with write access to this repository can run code there (keep the
collaborator list short, give the runner its own Windows account without admin rights, and never add a
`pull_request` trigger for it: on a public repository a fork's pull request would run its code on that PC). A release
needs that PC on and the runner started, and two builds use its CPU for an hour or more.

**B. GitHub-hosted runners, with the game's files from an encrypted source.**
The build needs Brawl's `sys/main.dol` and `files/module/*.rel` (25 MB). They would be stored encrypted (e.g. `age`
or `gpg --symmetric`, AES-256) somewhere the job can download them (a private repository's release, a storage
bucket), with the passphrase as an Actions secret, decrypted only in the job. Windows on `windows-latest`, Linux on
`ubuntu-24.04` (no WSL needed). Risks: the game's copyrighted files are copied off the owner's PC and kept online
(encrypted, but anyone who can read the secret can read them); any workflow change pushed by a collaborator can print
or upload the decrypted files, and GitHub masks only the secret, not files made with it; every release builds from
scratch on 4-core runners (the whole game translated and compiled twice for Windows and once for Linux: hours, near
the 6-hour job limit), and needs a cache to stay fast, which would then hold translated game code. It also needs the
same admin (to add the secret). Option A is the default for those reasons.

## Updating the core

1. Merge or commit the core change on brawlonline.net's `emissary-recomp` branch and push it to its
   `emissary-brawlonline` branch (never `main`).
2. In `brawl_online.lock`: the new `commit`, and `appVersion` = the Brawl Online release that commit contains.
3. Bump `VERSION`, commit, and release. A player's launcher updates to the new `/releases/latest` before its next
   game (never during one, nor between the games of a Ranked set).
