#!/usr/bin/env bash
set -euo pipefail

ROOT="$(pwd)/Kernel64Patcher"
SRC="$ROOT/src"
OUT="$ROOT/out"
RELEASE="$ROOT/release"

mkdir -p "$SRC" "$OUT" "$RELEASE"

########################################
# Config
########################################
JOBS=$(sysctl -n hw.ncpu)

# Universal flags (single build, no lipo needed)
export ARCH_FLAGS="-arch x86_64 -arch arm64"
export MIN_VER="-mmacosx-version-min=10.13"

export CC="clang"
export CFLAGS="-O2 $ARCH_FLAGS $MIN_VER"
export LDFLAGS="$ARCH_FLAGS $MIN_VER"

########################################
# Clone sources
########################################
cd "$SRC"

[ -d Kernel64Patcher ] || git clone https://github.com/palera1n/Kernel64Patcher.git
[ -d KPlooshFinder ]   || git clone --recursive https://github.com/palera1n/KPlooshFinder.git

cd KPlooshFinder
git submodule update --init --recursive
cd ..

########################################
# Build function
########################################
build_project() {
    NAME=$1
    DIR=$2

    echo "== Building $NAME =="

    pushd "$DIR" >/dev/null

    make clean || true
    make -j"$JOBS"

    if [ ! -f "$NAME" ]; then
        echo "❌ Build failed: $NAME not found"
        exit 1
    fi

    cp "$NAME" "$OUT/$NAME"
    popd >/dev/null
}

########################################
# Build all
########################################
build_project Kernel64Patcher "$SRC/Kernel64Patcher"
build_project KPlooshFinder "$SRC/KPlooshFinder"

########################################
# Generate embedded headers
########################################
xxd -i -n Kernel64Patcher_legacy "$OUT/Kernel64Patcher" > "$ROOT/Kernel64Patcher_legacy.h"
xxd -i -n KPlooshFinder "$OUT/KPlooshFinder" > "$ROOT/KPlooshFinder.h"

########################################
# Build wrapper (universal)
########################################
echo "== Building wrapper =="

clang "$ROOT/Kernel64Patcher_wrapper.c" \
  $ARCH_FLAGS $MIN_VER \
  -O2 \
  -o "$OUT/Kernel64Patcher_wrapper"

########################################
# Codesign (ad-hoc)
########################################
for BIN in \
  "$OUT/Kernel64Patcher" \
  "$OUT/KPlooshFinder" \
  "$OUT/Kernel64Patcher_wrapper"
do
  codesign --sign - --force "$BIN"
done

########################################
# Release bundle (fixed layout)
########################################
cp "$OUT/Kernel64Patcher_wrapper" "$RELEASE/Kernel64Patcher"
cp "$OUT/Kernel64Patcher"         "$RELEASE/Kernel64Patcher_legacy"
cp "$OUT/KPlooshFinder"           "$RELEASE/KPlooshFinder"

########################################
# Done
########################################
echo
echo "======================================"
echo "Build complete"
echo "Output:   $OUT"
echo "Release:  $RELEASE"
echo "======================================"
