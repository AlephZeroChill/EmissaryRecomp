#!/usr/bin/env bash
# The macOS program of a release (tools/release.py runs it on a Mac): builds <commit> of <repo> with Brawl Online's
# core at its brawl_online.lock, in ~/emissary-release-macos, and copies what the download needs to <out>:
# WiiCompiled, dsp_coef.bin, cacert.pem, wii_bootstrap/ and Sys/ (Brawl Online's region sets).
#   tools/release-macos.sh <repo> <extracted disc DATA> <out> <jobs> <tag> <commit>
# EMISSARY_RELEASE_IN_PLACE=1 builds <repo> itself, a checkout of <commit> (CI), in $EMISSARY_RELEASE_WORK/build.
# EMISSARY_RELEASE_CORE=<brawlonline.net checkout> links that core instead of the lock's (Brawl Online's staging).
# Apple silicon only (arm64). Needs Xcode's command line tools, CMake 3.25+, Ninja, Python 3.12+ and Git. Every
# library is built in (tools/build.py keeps Homebrew's and XQuartz's out), so the program needs only macOS's own.
set -euo pipefail
repo=$1 game=$2 out=$3 jobs=$4 tag=$5 commit=$6
work=${EMISSARY_RELEASE_WORK:-$HOME/emissary-release-macos}
src=$work/src build=$work/build

[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || { echo "the macOS program builds on an Apple silicon Mac" >&2; exit 1; }
mkdir -p "$work"
if [[ ${EMISSARY_RELEASE_IN_PLACE:-} == 1 ]]; then
    src=$(cd "$repo" && pwd)
    [[ $(git -C "$src" rev-parse HEAD) == "$(git -C "$src" rev-parse "$commit^{commit}")" ]] ||
        { echo "$src is not at $commit" >&2; exit 1; }
else
    if [[ ! -d $src/.git ]]; then
        git clone -q --no-checkout "$repo" "$src"
    fi
    git -C "$src" fetch -q "$repo" "$commit" 2>/dev/null || git -C "$src" fetch -q "$repo"
    git -C "$src" -c advice.detachedHead=false checkout -q -f "$commit"
    git -C "$src" clean -q -fdx -e /build/
fi
echo "macOS build of $tag: $(git -C "$src" log -1 --format='%h %s' | cut -c1-100)"
echo "macOS $(sw_vers -productVersion); $(clang --version | head -1); $(cmake --version | head -1)"

cmake "-DGAME=$game" "-DBUILD_DIR=$build" "-DJOBS=$jobs" "-DBRAWL_ONLINE=${EMISSARY_RELEASE_CORE:-lock}" \
    "-DBRAWL_ONLINE_DIR=$work/brawl-online" "-DRELEASE_VERSION=$tag" -P "$src/build.cmake"

native=$build/native
rm -rf "$out" && mkdir -p "$out"
cp "$native/WiiCompiled" "$native/dsp_coef.bin" "$native/cacert.pem" "$out/"
cp -R "$native/wii_bootstrap" "$native/Sys" "$out/"
# The libraries the build made itself (CMake's FetchContent, under _deps: libpng) ship beside the program; the build's
# rpath points into the build folder: @executable_path instead.
for lib in $(otool -L "$native/WiiCompiled" | tail -n +2 | awk '{print $1}' | sed -n 's|^@rpath/||p'); do
    built=$(find "$native/_deps" -name "$lib" -print -quit 2>/dev/null || true)
    [[ -n $built ]] || { echo "WiiCompiled needs @rpath/$lib, which the build did not make" >&2; exit 1; }
    cp -L "$built" "$out/$lib"
    install_name_tool -id "@rpath/$lib" "$out/$lib"
    echo "bundled $lib"
done
for rpath in $(otool -l "$out/WiiCompiled" | awk '/LC_RPATH/ {f=1} f && $1 == "path" {print $2; f=0}'); do
    install_name_tool -delete_rpath "$rpath" "$out/WiiCompiled"
done
install_name_tool -add_rpath @executable_path "$out/WiiCompiled"
# Only macOS's own libraries and frameworks besides those: anything else (Homebrew, XQuartz) is on this Mac only.
foreign=$(otool -L "$out/WiiCompiled" | tail -n +2 | awk '{print $1}' |
    grep -v -e '^/usr/lib/' -e '^/System/Library/' -e '^@rpath/' || true)
[[ -z $foreign ]] || { echo "WiiCompiled links libraries other Macs do not have: $foreign" >&2; exit 1; }
# Signed ad hoc (Apple silicon runs nothing unsigned; install_name_tool's changes undid the linker's signature). A copy
# the launcher downloads is not quarantined, so Gatekeeper does not check it.
for f in "$out"/*.dylib "$out/WiiCompiled"; do
    [[ -e $f ]] && codesign --force --sign - "$f"
done
echo "minimum macOS: $(otool -l "$out/WiiCompiled" | awk '/LC_BUILD_VERSION/ {f=1} f && /minos/ {print $2; exit}')"
echo "macOS program: $out"
