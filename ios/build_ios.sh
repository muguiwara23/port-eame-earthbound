#!/usr/bin/env bash
# Port/eame: Phase Distorter (EarthBound / Mother 2) as an unsigned iOS app,
# built with the Port/eame kit (port-eame-kit/tools/ios_env.sh).
#
# Runs on a Mac with Xcode (the kit's GitHub action runs it): builds SDL3 for
# iOS from its pinned release, compiles the game for arm64 iOS 16+, and
# packages an unsigned .ipa from porteame.json under build/ios/release. No ROM,
# asset pack or save is used or included; the player signs the .ipa with their
# own tool.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
export PORTEAME_WORK="${PORTEAME_WORK:-$root/build/ios}"
source "$root/port-eame-kit/tools/ios_env.sh"

porteame_build_sdl3

cmake -S "$root" -B "$PORTEAME_WORK/build" "${porteame_ios_cmake[@]}" \
    -DEB_BUILD_TESTS=OFF -DSDL3_DIR="$PORTEAME_SDL3_DIR" \
    -DCMAKE_EXE_LINKER_FLAGS="$PORTEAME_IOS_LINK"
cmake --build "$PORTEAME_WORK/build" --target eb_cpp --parallel "$porteame_jobs"

binary="$PORTEAME_WORK/build/cpp/eb_cpp"
porteame_check_binary "$binary"
porteame_package "$root/porteame.json" "$binary"
