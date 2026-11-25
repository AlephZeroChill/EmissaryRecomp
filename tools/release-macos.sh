#!/usr/bin/env bash
# The macOS program of a release (tools/release.py runs it on a Mac): builds <commit> of <repo> with Brawl Online's
# core at its brawl_online.lock, in ~/emissary-release-macos, and copies what the download needs to <out>: Emissary.app
# (the program, named and with its icon for macOS), dsp_coef.bin, cacert.pem, wii_bootstrap/ and Sys/ (Brawl Online's
# region sets).
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
# The program as a macOS app, Emissary.app: the Dock, the menu bar and Cmd-Tab name it and show its icon (a bare program
# shows as a terminal). Its files stay beside the bundle, where the runtime looks for them (host_platform.cpp).
app=$out/Emissary.app
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
program=$app/Contents/MacOS/Emissary
cp "$native/WiiCompiled" "$program"
cp "$src/wiicompiled/runtime/assets/icon/emissary.icns" "$app/Contents/Resources/Emissary.icns"
version=${tag#v}
cat >"$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleDisplayName</key><string>Emissary</string>
  <key>CFBundleExecutable</key><string>Emissary</string>
  <key>CFBundleIconFile</key><string>Emissary</string>
  <key>CFBundleIdentifier</key><string>net.brawlonline.emissary</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>Emissary</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${version%%-*}</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>EmissaryRecomp by kalomaze, a fork of BrawlRecompiled by JosiahG, built on WiiCompiled by patchzyy. GPLv3.</string>
</dict>
</plist>
PLIST
plutil -lint "$app/Contents/Info.plist" >/dev/null
cp "$native/dsp_coef.bin" "$native/cacert.pem" "$out/"
cp -R "$native/wii_bootstrap" "$native/Sys" "$out/"
# The libraries the build made itself (CMake's FetchContent, under _deps: libpng, when it is shared) go in the bundle's
# Frameworks; the build's rpath points into the build folder: @executable_path/../Frameworks instead.
for lib in $(otool -L "$program" | tail -n +2 | awk '{print $1}' | sed -n 's|^@rpath/||p'); do
    built=$(find "$native/_deps" -name "$lib" -print -quit 2>/dev/null || true)
    [[ -n $built ]] || { echo "the program needs @rpath/$lib, which the build did not make" >&2; exit 1; }
    mkdir -p "$app/Contents/Frameworks"
    cp -L "$built" "$app/Contents/Frameworks/$lib"
    install_name_tool -id "@rpath/$lib" "$app/Contents/Frameworks/$lib"
    echo "bundled $lib"
done
for rpath in $(otool -l "$program" | awk '/LC_RPATH/ {f=1} f && $1 == "path" {print $2; f=0}'); do
    install_name_tool -delete_rpath "$rpath" "$program"
done
install_name_tool -add_rpath @executable_path/../Frameworks "$program"
# Only macOS's own libraries and frameworks besides those: anything else (Homebrew, XQuartz) is on this Mac only.
foreign=$(otool -L "$program" | tail -n +2 | awk '{print $1}' |
    grep -v -e '^/usr/lib/' -e '^/System/Library/' -e '^@rpath/' || true)
[[ -z $foreign ]] || { echo "the program links libraries other Macs do not have: $foreign" >&2; exit 1; }
# Signed ad hoc (Apple silicon runs nothing unsigned; install_name_tool's changes undid the linker's signature): the
# libraries, then the app (its program, Info.plist and icon). A copy the launcher downloads is not quarantined, so
# Gatekeeper does not check it.
for f in "$app"/Contents/Frameworks/*.dylib; do
    [[ -e $f ]] && codesign --force --sign - "$f"
done
codesign --force --sign - "$app"
codesign --verify --strict "$app"
echo "minimum macOS: $(otool -l "$program" | awk '/LC_BUILD_VERSION/ {f=1} f && /minos/ {print $2; exit}')"
echo "macOS program: $out"
