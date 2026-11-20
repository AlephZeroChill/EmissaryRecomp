#!/usr/bin/env bash
# Installs what the Linux release build needs on Ubuntu 24.04 (run as root). Releases are built on 24.04 so they run on
# glibc 2.38+ (Ubuntu 24.04, Debian 13, SteamOS 3.6, Fedora 39+, Arch...); a newer system would raise that floor.
# The release machine runs it inside a WSL distro made from Ubuntu's own WSL image (docs/releases.md):
#   wsl --import EmissaryRecomp-Build <folder> ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz --version 2
#   wsl -d EmissaryRecomp-Build -u root -- bash tools/linux-build-deps.sh
set -euo pipefail
. /etc/os-release
[[ ${VERSION_ID:-} == 24.04 ]] || echo "warning: releases are built on Ubuntu 24.04, this is ${PRETTY_NAME:-unknown}" >&2
export DEBIAN_FRONTEND=noninteractive
# A build machine: no fsync after every unpacked file (in a WSL disk image those made the install take over 30 minutes).
echo force-unsafe-io >/etc/dpkg/dpkg.cfg.d/unsafe-io
apt-get update -q
# README.md's list (SDL, Dawn and the runtime), git, and patchelf (the program finds its libpng beside it: $ORIGIN).
apt-get install -y -q --no-install-recommends ca-certificates cmake python3 git clang lld ninja-build build-essential \
  pkg-config libicu-dev patchelf \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev libxrender-dev \
  libxinerama-dev libwayland-dev wayland-protocols libdecor-0-dev libxkbcommon-dev libgl-dev libegl-dev libgbm-dev \
  libdrm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libusb-1.0-0-dev libibus-1.0-dev
echo "clang: $(clang --version | head -1); cmake: $(cmake --version | head -1); python: $(python3 --version)"
# tools/release-linux.sh clones the release machine's checkout (/mnt/<drive>/...), which belongs to another user.
git config --system --add safe.directory '*'
