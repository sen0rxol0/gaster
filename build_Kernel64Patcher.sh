#!/usr/bin/env bash
set -euo pipefail

ROOT="$(pwd)/Kernel64Patcher"
SRC="$ROOT/src"
BUILD="$ROOT/build"
UNIVERSAL="$ROOT/universal"
RELEASE="$ROOT/release"

mkdir -p "$SRC" "$BUILD" "$UNIVERSAL" "$RELEASE"

########################################
# Dependencies
########################################
if command -v brew >/dev/null; then
    brew install cmake || true
fi

########################################
# Clone sources
########################################
cd "$SRC"

[ -d Kernel64Patcher ] || git clone https://github.com/palera1n/Kernel64Patcher.git

if [ ! -d KPlooshFinder ]; then
    git clone --recursive https://github.com/palera1n/KPlooshFinder.git
else
    cd KPlooshFinder
    git submodule update --init --recursive
    cd ..
fi

########################################
# Generic CMake builder
########################################
build_arch() {
    NAME=$1
    SRC_PATH=$2
    ARCH=$3
    TARGET=$4

    BUILD_PATH="$BUILD/${NAME}_${ARCH}"
    INSTALL="$BUILD_PATH/install"

    rm -rf "$BUILD_PATH"
    mkdir -p "$BUILD_PATH"
    cd "$BUILD_PATH"

    cmake "$SRC_PATH" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL"

    cmake --build . --parallel
    cmake --install .

    # FIX: use -perm /111 (POSIX-compatible) instead of deprecated +111
    find "$INSTALL" -type f -perm /111 | head -n1
}

########################################
# Build legacy Kernel64Patcher
########################################
echo "== Building Kernel64Patcher =="

K64_X86=$(build_arch k64 "$SRC/Kernel64Patcher" x86_64 10.13)
K64_ARM=$(build_arch k64 "$SRC/Kernel64Patcher" arm64 11.0)

lipo -create "$K64_X86" "$K64_ARM" -output "$UNIVERSAL/Kernel64Patcher_legacy"


########################################
# Build KPlooshFinder
########################################
echo "== Building KPlooshFinder =="

KPF_X86=$(build_arch kpf "$SRC/KPlooshFinder" x86_64 10.13)
KPF_ARM=$(build_arch kpf "$SRC/KPlooshFinder" arm64 11.0)

lipo -create "$KPF_X86" "$KPF_ARM" -output "$UNIVERSAL/KPlooshFinder"


xxd -i "$UNIVERSAL/Kernel64Patcher_legacy" > "$ROOT/Kernel64Patcher_legacy.h"
xxd -i "$UNIVERSAL/KPlooshFinder" > "$ROOT/KPlooshFinder.h"

########################################
# Build wrapper
########################################
echo "== Building wrapper =="

clang "$ROOT/Kernel64Patcher_wrapper.c" \
  -O2 \
  -arch x86_64 -mmacosx-version-min=10.13 \
  -arch arm64  -mmacosx-version-min=11.0 \
  -o "$UNIVERSAL/Kernel64Patcher"

# FIX: ad-hoc sign the universal wrapper so it executes on Apple Silicon
#      under default SIP/Gatekeeper settings without quarantine errors.
codesign --sign - --force "$UNIVERSAL/Kernel64Patcher"
codesign --sign - --force "$UNIVERSAL/Kernel64Patcher_legacy"
codesign --sign - --force "$UNIVERSAL/KPlooshFinder"

########################################
# Assemble release bundle
########################################
echo "== Creating release bundle =="

cp "$UNIVERSAL/Kernel64Patcher"        "$RELEASE/"
cp "$UNIVERSAL/Kernel64Patcher_legacy" "$RELEASE/"
cp "$UNIVERSAL/KPlooshFinder"          "$RELEASE/"

echo
echo "======================================"
echo "DONE! Release folder:"
echo "$RELEASE"
echo "======================================"
