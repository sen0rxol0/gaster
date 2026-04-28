#!/usr/bin/env bash
set -euo pipefail

ROOT="$(pwd)/Kernel64Patcher-build"
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

    find "$INSTALL" -type f -perm +111 | head -n1
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


xxd -i "$UNIVERSAL/Kernel64Patcher_legacy" > "$BUILD/Kernel64Patcher_legacy.h"
xxd -i "$UNIVERSAL/KPlooshFinder" > "$BUILD/KPlooshFinder.h"

########################################
# Build wrapper
########################################
echo "== Building wrapper =="

cat > "$BUILD/wrapper.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "Kernel64Patcher_legacy.h"
#include "KPlooshFinder.h"

static int write_temp_exec(const char *name,
                           unsigned char *data,
                           unsigned int size,
                           char *out_path)
{
    sprintf(out_path, "/tmp/%s_%d", name, getpid());

    int fd = open(out_path, O_CREAT | O_WRONLY, 0755);
    if (fd < 0) return -1;

    write(fd, data, size);
    close(fd);
    chmod(out_path, 0755);
    return 0;
}

int get_darwin_major(const char *kernel_path)
{
    FILE *f = fopen(kernel_path, "rb");
    if (!f) return -1;

    char buf[0x4000];
    fread(buf, 1, sizeof(buf), f);
    fclose(f);

    const char *needle = "Darwin Kernel Version ";
    char *p = memmem(buf, sizeof(buf), needle, strlen(needle));
    if (!p) return -1;

    return atoi(p + strlen(needle));
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <kernelcache> [args...]\n", argv[0]);
        return 1;
    }

    int darwin = get_darwin_major(argv[1]);
    if (darwin < 0) {
        printf("Failed to detect Darwin version\n");
        return 1;
    }

    char tool_path[256];

    if (darwin >= 22) {
        printf("Using embedded KPlooshFinder\n");
        write_temp_exec("kpf", KPlooshFinder, KPlooshFinder_len, tool_path);
    } else {
        printf("Using embedded Kernel64Patcher\n");
        write_temp_exec("k64", Kernel64Patcher_legacy, Kernel64Patcher_legacy_len, tool_path);
    }

    execv(tool_path, argv);
    perror("exec failed");
    return 1;
}
EOF

clang "$BUILD/wrapper.c" \
  -O2 \
  -arch x86_64 -mmacosx-version-min=10.13 \
  -arch arm64  -mmacosx-version-min=11.0 \
  -o "$UNIVERSAL/Kernel64Patcher"

########################################
# Assemble release bundle
########################################
echo "== Creating release bundle =="

cp "$UNIVERSAL/Kernel64Patcher" "$RELEASE/"
cp "$UNIVERSAL/Kernel64Patcher_legacy" "$RELEASE/"
cp "$UNIVERSAL/KPlooshFinder" "$RELEASE/"

echo
echo "======================================"
echo "DONE! Release folder:"
echo "$RELEASE"
echo "======================================"
