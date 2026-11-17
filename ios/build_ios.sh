#!/usr/bin/env bash
# Port/eame: Phase Distorter (EarthBound / Mother 2) as an unsigned iOS app.
#
# Runs on a Mac with Xcode (GitHub's macOS runner): builds SDL3 for iOS from
# its pinned release, compiles the game for arm64 iOS 16+, and packages an
# unsigned .ipa under build/ios/release. No ROM, asset pack or save is used or
# included; the player signs the .ipa with their own tool.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="${PORTEAME_WORK:-$root/build/ios}"
version="${PORTEAME_VERSION:-0.5-ios-dev}"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 3)"
minimum=16.0

sdl_version=3.4.16
sdl_url="https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/SDL3-${sdl_version}.tar.gz"
sdl_sha256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68

[[ "$(uname -s)" == Darwin ]] || { echo "iOS builds need macOS and Xcode." >&2; exit 2; }
xcodebuild -version
echo "iOS SDK: $(xcrun --sdk iphoneos --show-sdk-version) at $(xcrun --sdk iphoneos --show-sdk-path)"

ios_cmake=(-G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos
           -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$minimum"
           -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCMAKE_BUILD_TYPE=Release)

# --- SDL3, static, for iOS ---------------------------------------------------
deps="$work/deps"
sdl_source="$deps/SDL3-$sdl_version"
sdl_install="$deps/sdl3"
mkdir -p "$deps"
if [[ ! -f "$sdl_install/lib/libSDL3.a" ]]; then
    curl -fsSL "$sdl_url" -o "$deps/sdl3.tar.gz"
    echo "$sdl_sha256  $deps/sdl3.tar.gz" | shasum -a 256 -c -
    tar -xzf "$deps/sdl3.tar.gz" -C "$deps"
    cmake -S "$sdl_source" -B "$deps/sdl3-build" "${ios_cmake[@]}" \
        -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
        -DCMAKE_INSTALL_PREFIX="$sdl_install"
    cmake --build "$deps/sdl3-build" --parallel "$jobs"
    cmake --install "$deps/sdl3-build"
fi

# The frameworks the static SDL3 records for this SDK, plus OpenGL ES.
frameworks="$(python3 - "$sdl_install/lib/pkgconfig/sdl3.pc" <<'PY'
import sys
words = [w for line in open(sys.argv[1]) if line.startswith(("Libs:", "Libs.private:"))
         for w in line.split(":", 1)[1].split()]
out = []
for i, w in enumerate(words):
    if w in ("-framework", "-weak_framework") and i + 1 < len(words):
        out += [w, words[i + 1]]
    elif w.startswith("-Wl,") and "framework" in w:
        out.append(w)
    elif w.startswith("-l") and w not in ("-lSDL3", "-lm"):
        out.append(w)
if not any("UIKit" in w for w in out):
    sys.exit("sdl3.pc lists no iOS frameworks: " + " ".join(words))
if "OpenGLES" not in " ".join(out):
    out += ["-framework", "OpenGLES"]
print(" ".join(out))
PY
)"
echo "link: $frameworks"

# --- the game -------------------------------------------------------------------
cmake -S "$root" -B "$work/build" "${ios_cmake[@]}" \
    -DEB_BUILD_TESTS=OFF -DSDL3_DIR="$sdl_install/lib/cmake/SDL3" \
    -DCMAKE_EXE_LINKER_FLAGS="$frameworks"
cmake --build "$work/build" --target eb_cpp --parallel "$jobs"

binary="$work/build/cpp/eb_cpp"
lipo -info "$binary"
otool -l "$binary" | awk '/LC_BUILD_VERSION/{f=1} f&&/(platform|minos|sdk)/{print} f&&/ntools/{f=0}'

# --- package -------------------------------------------------------------------
python3 "$root/ios/package_ios.py" --binary "$binary" --out "$work/release" --version "$version" \
    --sdl-license "$sdl_source/LICENSE.txt"
