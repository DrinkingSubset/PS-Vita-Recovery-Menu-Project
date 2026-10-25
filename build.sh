#!/usr/bin/env bash
# build.sh - build PS Vita Recovery Menu into build/VitaRecovery.vpk
#
# Usage (run from anywhere):
#   ./build.sh           normal build (fast, only rebuilds what changed)
#   ./build.sh clean     delete build/ first, then do a full rebuild
#   ./build.sh bump      increment VERSION by one step first (1.1.0 -> 1.1.1)
#   ./build.sh clean bump   both
#
# Only use "bump" when the code changed and you are making a NEW build.

set -e

# Always work from the folder this script lives in
cd "$(dirname "$(readlink -f "$0")")"
PROJECT_DIR="$(pwd)"

# The VitaSDK packer fails on folder paths that contain spaces
case "$PROJECT_DIR" in
    *" "*)
        echo "ERROR: this folder path contains a space:"
        echo "  $PROJECT_DIR"
        echo "Rename or move the folder so the path has no spaces, then run again."
        exit 1
        ;;
esac

# Locate the SDK
if [ -z "$VITASDK" ]; then
    export VITASDK=/usr/local/vitasdk
fi
export PATH="$VITASDK/bin:$PATH"

if [ ! -f "$VITASDK/share/vita.toolchain.cmake" ]; then
    echo "ERROR: VitaSDK not found at $VITASDK"
    exit 1
fi

# Options
DO_CLEAN=0
DO_BUMP=0
for arg in "$@"; do
    case "$arg" in
        clean) DO_CLEAN=1 ;;
        bump)  DO_BUMP=1 ;;
        *) echo "Unknown option: $arg  (use: clean, bump)"; exit 1 ;;
    esac
done

if [ "$DO_BUMP" -eq 1 ]; then
    ./bump_version.sh
fi

if [ "$DO_CLEAN" -eq 1 ]; then
    echo "Removing old build/ folder..."
    rm -rf build
fi

VERSION="$(tr -d '[:space:]' < VERSION)"
echo "=== Building PS Vita Recovery Menu v$VERSION ==="

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build

if [ ! -f build/VitaRecovery.vpk ]; then
    echo "ERROR: build finished but build/VitaRecovery.vpk was not created."
    exit 1
fi

# Keep a copy with the version in its name, next to this script
cp -f build/VitaRecovery.vpk "VitaRecovery_v$VERSION.vpk"

echo
echo "=== DONE: v$VERSION ==="
ls -la "VitaRecovery_v$VERSION.vpk"
echo "Full path: $PROJECT_DIR/VitaRecovery_v$VERSION.vpk"
