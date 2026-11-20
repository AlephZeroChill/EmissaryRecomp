#!/usr/bin/env bash
# The Linux program of a release (tools/release.py runs it, in WSL on Windows): builds <commit> of <repo> with Brawl
# Online's core at its brawl_online.lock, in ~/emissary-release (Linux's own disk: a build on /mnt/<drive> is many
# times slower), and copies what the download needs to <out>: WiiCompiled (finding its libraries beside it), the
# libraries the build made itself, if it links any, dsp_coef.bin, cacert.pem, wii_bootstrap/ and Sys/ (Brawl Online's region sets).
#   tools/release-linux.sh <repo> <extracted disc DATA> <out> <jobs> <tag> <commit>
# Build on Ubuntu 24.04 (tools/linux-build-deps.sh): the program needs the glibc and libstdc++ it was built with or newer.
set -euo pipefail
repo=$1 game=$2 out=$3 jobs=$4 tag=$5 commit=$6
work=${EMISSARY_RELEASE_WORK:-$HOME/emissary-release}
src=$work/src build=$work/build

mkdir -p "$work"
if [[ ! -d $src/.git ]]; then
    git clone -q --no-checkout "$repo" "$src"
fi
git -C "$src" fetch -q "$repo" "$commit" 2>/dev/null || git -C "$src" fetch -q "$repo"
git -C "$src" -c advice.detachedHead=false checkout -q -f "$commit"
git -C "$src" clean -q -fdx -e /build/
echo "Linux build of $tag: $(git -C "$src" log -1 --format='%h %s' | cut -c1-100)"
echo "$(. /etc/os-release && echo "$PRETTY_NAME"); $(clang --version | head -1); $(cmake --version | head -1)"

cmake "-DGAME=$game" "-DBUILD_DIR=$build" "-DJOBS=$jobs" -DBRAWL_ONLINE=lock "-DBRAWL_ONLINE_DIR=$work/brawl-online" \
    "-DRELEASE_VERSION=$tag" -P "$src/build.cmake"

native=$build/native
rm -rf "$out" && mkdir -p "$out"
cp "$native/WiiCompiled" "$native/dsp_coef.bin" "$native/cacert.pem" "$out/"
cp -r "$native/wii_bootstrap" "$native/Sys" "$out/"
# The libraries the build made itself (CMake's FetchContent, under _deps) ship beside the program; the system's are
# the player's. The build's RUNPATH points into the build folder: $ORIGIN instead.
for lib in $(readelf -d "$native/WiiCompiled" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    built=$(find "$native/_deps" -name "$lib" -print -quit 2>/dev/null || true)
    if [[ -n $built ]]; then
        cp -L "$built" "$out/$lib"
        echo "bundled $lib"
    fi
done
patchelf --set-rpath '$ORIGIN' "$out/WiiCompiled"
for so in "$out"/*.so*; do [[ -e $so ]] && patchelf --set-rpath '$ORIGIN' "$so"; done

echo "needs: $(readelf -d "$out/WiiCompiled" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
echo "glibc: $(objdump -T "$out/WiiCompiled" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)," \
     "libstdc++: $(objdump -T "$out/WiiCompiled" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)"
missing=$(cd "$out" && ldd ./WiiCompiled | grep 'not found' || true)
[[ -z $missing ]] || { echo "unresolved libraries: $missing" >&2; exit 1; }
echo "Linux program: $out"
